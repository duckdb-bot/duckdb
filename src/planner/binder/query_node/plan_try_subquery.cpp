#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <algorithm>

namespace duckdb {

static unique_ptr<Expression> WrapInTry(unique_ptr<Expression> child) {
	auto result = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_TRY, child->GetReturnType());
	result->GetChildrenMutable().push_back(std::move(child));
	return std::move(result);
}

//! Match the use-site re-raise PlanSubquery creates for a subquery planned with an exposed error flag
static bool IsTryErrorRethrow(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	return expr.Cast<BoundFunctionExpression>().Function().GetName() == TryRethrowFun::Name;
}

//! Collect the error flags of re-raises in unconditionally evaluated positions that no TRY or
//! HAS_ERROR inside the expression consumes. A flag from a conditionally evaluated position (a CASE
//! branch, a later COALESCE argument, the short-circuited operand of a selection conjunction) must
//! not join the gate: the gate fires eagerly, while evaluation may never reach that re-raise. The
//! split follows the executor: value evaluation computes every operand of conjunctions and IN lists,
//! while selection evaluation (a CASE WHEN condition) short-circuits conjunctions. Function arguments
//! count as unconditional even though the executor can skip later arguments after a constant NULL:
//! that eager reading of a nested subquery error is accepted and pinned in tests.
static void CollectTryErrorRethrowFlags(const Expression &expr, bool select_context,
                                        vector<unique_ptr<Expression>> &flags) {
	if (IsTryErrorRethrow(expr)) {
		flags.push_back(expr.Cast<BoundFunctionExpression>().GetChildren()[0]->Copy());
		return;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CASE:
		// only the first WHEN condition is evaluated unconditionally, and it is evaluated as a selection
		CollectTryErrorRethrowFlags(*expr.Cast<BoundCaseExpression>().CaseChecks()[0].when_expr, true, flags);
		return;
	case ExpressionClass::BOUND_CONJUNCTION:
		if (select_context) {
			// selection evaluation short-circuits, so only the first operand always evaluates
			CollectTryErrorRethrowFlags(*expr.Cast<BoundConjunctionExpression>().GetChildren()[0], true, flags);
			return;
		}
		break;
	case ExpressionClass::BOUND_OPERATOR:
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_TRY:
		case ExpressionType::OPERATOR_HAS_ERROR:
			// the capture consumes re-raises below it
			return;
		case ExpressionType::OPERATOR_COALESCE:
			CollectTryErrorRethrowFlags(*expr.Cast<BoundOperatorExpression>().GetChildren()[0], false, flags);
			return;
		default:
			break;
		}
		break;
	default:
		break;
	}
	// everything else evaluates its children as values before producing a result
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { CollectTryErrorRethrowFlags(child, false, flags); });
}

static unique_ptr<Expression> OrErrorFlags(unique_ptr<Expression> gate, unique_ptr<Expression> flag) {
	if (!gate) {
		return flag;
	}
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(gate), std::move(flag));
}

//! Find the aggregate node of the select node by its recorded table index. The search traverses
//! joins that subquery flattening may have inserted between the select list and the aggregate.
static optional_ptr<LogicalAggregate> FindAggregateOperator(LogicalOperator &op, TableIndex aggregate_index) {
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &aggr = op.Cast<LogicalAggregate>();
		if (aggr.aggregate_index == aggregate_index) {
			return &aggr;
		}
	}
	for (auto &child : op.children) {
		auto result = FindAggregateOperator(*child, aggregate_index);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

//! Wrap the inputs of the gated aggregates in TRY and aggregate HAS_ERROR probes over them. Sets gate to a column
//! reference to the probe aggregate, or to nullptr when the aggregates have no inputs. Returns false when a shape
//! cannot be gated and strict is unset (throws otherwise).
static bool GateTryAggregates(ClientContext &context, const Expression &location, LogicalAggregate &aggregate,
                              const vector<idx_t> &aggregate_positions, bool strict, unique_ptr<Expression> &gate) {
	// validate every position before mutating anything, so a non-strict caller can still fall back to
	// the plain subquery
	for (auto position : aggregate_positions) {
		auto &aggr_expr = aggregate.expressions[position]->Cast<BoundAggregateExpression>();
		if (aggr_expr.GetFilter()) {
			if (strict) {
				throw BinderException(location, "TRY over a subquery with an aggregate FILTER clause is not supported");
			}
			return false;
		}
		if (aggr_expr.GetOrderBys()) {
			if (strict) {
				throw BinderException(location, "TRY over a subquery with an ordered aggregate is not supported");
			}
			return false;
		}
		if (aggr_expr.StateExportMode() != AggregateStateExportMode::NONE) {
			if (strict) {
				throw BinderException(location,
				                      "TRY over a subquery with an exported aggregate state is not supported");
			}
			return false;
		}
		for (auto &child : aggr_expr.GetChildren()) {
			if (child->IsVolatile()) {
				if (strict) {
					throw BinderException(location, "TRY can not be used in combination with a volatile function");
				}
				return false;
			}
		}
	}
	// wrap the aggregate inputs in TRY and collect HAS_ERROR probes over the same inputs
	for (auto position : aggregate_positions) {
		auto &aggr_expr = aggregate.expressions[position]->Cast<BoundAggregateExpression>();
		for (auto &child : aggr_expr.GetChildrenMutable()) {
			auto probe = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_HAS_ERROR, LogicalType::BOOLEAN);
			probe->GetChildrenMutable().push_back(child->Copy());
			gate = OrErrorFlags(std::move(gate), std::move(probe));
			child = WrapInTry(std::move(child));
		}
	}
	if (!gate) {
		// aggregates without inputs, such as count(*), have nothing to mask
		return true;
	}
	// aggregate the probes, so that an error in any input row raises the gate
	auto &system_catalog = Catalog::GetSystemCatalog(context);
	auto &bool_or = system_catalog.GetEntry<AggregateFunctionCatalogEntry>(
	    context, QualifiedName(system_catalog.GetName(), DEFAULT_SCHEMA, "bool_or"));
	vector<pair<Identifier, unique_ptr<Expression>>> gate_children;
	gate_children.emplace_back(Identifier(), std::move(gate));
	ErrorData error;
	FunctionBinder function_binder(context);
	auto gate_aggregate = function_binder.BindAggregateFunction(bool_or, std::move(gate_children), error);
	if (!gate_aggregate) {
		error.AddQueryLocation(location.GetQueryLocation());
		error.Throw();
	}
	ColumnBinding gate_binding(aggregate.aggregate_index, ProjectionIndex(aggregate.expressions.size()));
	aggregate.expressions.push_back(std::move(gate_aggregate));
	gate = make_uniq<BoundColumnRefExpression>(LogicalType::BOOLEAN, gate_binding);
	return true;
}

//! The select item of a subquery under TRY, located in its plan
struct TrySubquerySelectItem {
	//! The select list projection of the select node
	optional_ptr<LogicalProjection> projection;
	//! The column of the item in that projection
	ColumnBinding binding;
	//! Whether ORDER BY, DISTINCT or a prune projection above the projection references its columns
	bool has_result_modifiers = false;
};

//! Walk from the plan root to the select list projection recorded for the select node
static TrySubquerySelectItem FindTrySubquerySelectItem(LogicalOperator &root, const TrySubqueryPlanIndexes &indexes) {
	TrySubquerySelectItem result;
	result.binding = root.GetColumnBindings()[0];
	reference<LogicalOperator> current(root);
	while (!result.projection) {
		auto &current_op = current.get();
		switch (current_op.type) {
		case LogicalOperatorType::LOGICAL_ORDER_BY:
		case LogicalOperatorType::LOGICAL_TOP_N:
			// these modifiers reference the select list column
			result.has_result_modifiers = true;
			current = *current_op.children[0];
			continue;
		case LogicalOperatorType::LOGICAL_LIMIT:
			// constant LIMIT and OFFSET forward the child columns without referencing them. A LIMIT computed by a
			// scalar subquery adds a hidden select list entry and prunes. USING SAMPLE attaches below the select
			// list projection and never appears on this path
			current = *current_op.children[0];
			continue;
		case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
			current = *current_op.children[1];
			continue;
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			auto &projection = current_op.Cast<LogicalProjection>();
			if (projection.table_index == indexes.prune_index) {
				// pruning accompanies hidden select list entries (ORDER BY columns or a LIMIT computed by a scalar
				// subquery), the prune projection forwards the select list columns
				result.has_result_modifiers = true;
				result.binding =
				    projection.expressions[result.binding.column_index]->Cast<BoundColumnRefExpression>().Binding();
				current = *projection.children[0];
				continue;
			}
			if (indexes.distinct_index.IsValid() && projection.table_index == indexes.distinct_index) {
				// DISTINCT: a projection over the aggregate that deduplicates the select list
				result.has_result_modifiers = true;
				// look through the DISTINCT to the select list column it forwards
				auto &distinct_aggregate = projection.children[0]->Cast<LogicalAggregate>();
				auto distinct_binding =
				    projection.expressions[result.binding.column_index]->Cast<BoundColumnRefExpression>().Binding();
				reference<Expression> source(distinct_binding.table_index == distinct_aggregate.group_index
				                                 ? *distinct_aggregate.groups[distinct_binding.column_index]
				                                 : *distinct_aggregate.expressions[distinct_binding.column_index]);
				if (source.get().GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
					source = *source.get().Cast<BoundAggregateExpression>().GetChildren()[0];
				}
				result.binding = source.get().Cast<BoundColumnRefExpression>().Binding();
				current = *distinct_aggregate.children[0];
				continue;
			}
			if (projection.table_index != indexes.projection_index) {
				throw InternalException("TRY subquery: unexpected projection above the select list");
			}
			result.projection = &projection;
			continue;
		}
		default:
			throw InternalException("TRY subquery: unexpected operator above the select list projection");
		}
	}
	return result;
}

//! Push TRY onto the maskable positions of the select item, before the subquery is flattened. A plain projection
//! gets TRY on the projected expression, which yields NULL under the one-row contract of a scalar subquery. An
//! aggregating item is gated on HAS_ERROR probes of the aggregate inputs, so an input error anywhere turns the
//! aggregated result into NULL instead of a partially aggregated value, while an error inside aggregate execution
//! itself propagates. Nested scalar subqueries were planned with an exposed error flag and re-raise their errors
//! into these positions, unless their shape made them fall back to the plain subquery. Errors outside the maskable
//! positions (the WHERE clause, aggregate or window arguments, the one-row cardinality check) propagate deliberately,
//! and volatile computation inside a maskable position is rejected. With an exposed error flag the item becomes a
//! struct of the masked value and the flag for the use site to re-raise, and that struct type is returned. The
//! optional stays empty without a flag, and when the shape is not coverable and the lowering may fall back.
static optional<LogicalType> LowerTrySubquery(ClientContext &context, Binder &binder, const Expression &location,
                                              const TrySubqueryPlanIndexes &indexes, TrySubquerySelectItem &item,
                                              TrySubqueryLowering lowering) {
	// a fallback to the plain subquery would strand the re-raises of nested subqueries that were already planned
	// with an exposed flag, so such shapes are reported even when a fallback is allowed
	const bool strict = lowering != TrySubqueryLowering::TRANSPORT || indexes.nested_transport;
	const bool expose_error_flag = lowering != TrySubqueryLowering::DIRECT;
	if (expose_error_flag && item.has_result_modifiers) {
		// the modifiers reference or deduplicate the select list column. A struct transport would leave their
		// references scalar or change DISTINCT semantics (an error-masked NULL must not be distinguishable from a
		// plain NULL)
		if (strict) {
			throw BinderException(location, "TRY over a subquery with result modifiers is only supported when the "
			                                "subquery is the direct argument of TRY");
		}
		return optional<LogicalType>();
	}

	// classify the projected expression against the recorded indexes of the select node, so that the lowering
	// never crosses into derived tables, flattened subqueries or other query levels
	auto &select_projection = *item.projection;
	auto &expr = select_projection.expressions[item.binding.column_index];
	vector<ColumnBinding> refs;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(*expr, [&](const BoundColumnRefExpression &colref) {
		if (colref.Depth() == 0) {
			refs.push_back(colref.Binding());
		}
	});
	vector<idx_t> aggregate_positions;
	for (auto &ref : refs) {
		if (ref.table_index == indexes.window_index) {
			if (strict) {
				throw BinderException(location,
				                      "TRY over a subquery with a window function in its select list is not supported");
			}
			return optional<LogicalType>();
		}
		for (auto &unnest_index : indexes.unnest_indexes) {
			if (ref.table_index == unnest_index) {
				if (strict) {
					throw BinderException(location,
					                      "TRY over a subquery with UNNEST in its select list is not supported");
				}
				return optional<LogicalType>();
			}
		}
		if (ref.table_index == indexes.aggregate_index) {
			aggregate_positions.push_back(ref.column_index);
		}
	}
	// error_flag is only produced when the use site re-raises captured errors
	unique_ptr<Expression> error_flag;
	if (!aggregate_positions.empty()) {
		// the select item aggregates in this node: gate it on HAS_ERROR probes of the aggregate inputs
		auto aggregate = FindAggregateOperator(*select_projection.children[0], indexes.aggregate_index);
		if (!aggregate) {
			throw InternalException("TRY subquery: the aggregate node was not found below the select list");
		}
		std::sort(aggregate_positions.begin(), aggregate_positions.end());
		aggregate_positions.erase(std::unique(aggregate_positions.begin(), aggregate_positions.end()),
		                          aggregate_positions.end());
		unique_ptr<Expression> gate;
		if (!GateTryAggregates(context, location, *aggregate, aggregate_positions, strict, gate)) {
			return optional<LogicalType>();
		}
		// re-raises of nested subqueries next to the aggregates have no consuming TRY above them: their
		// flags join the gate, and the gated CASE never evaluates the re-raise on an error row; a
		// re-raise in a conditionally evaluated position stays out of the gate and raises if reached
		vector<unique_ptr<Expression>> rethrow_flags;
		CollectTryErrorRethrowFlags(*expr, false, rethrow_flags);
		for (auto &flag : rethrow_flags) {
			gate = OrErrorFlags(std::move(gate), std::move(flag));
		}
		if (gate) {
			if (expose_error_flag) {
				// the error flag carries the gate; the CASE below still guards the projected
				// expression, whose evaluation can depend on the gated aggregate results
				error_flag = gate->Copy();
			}
			// gate the projected expression: an input error turns the aggregated result into NULL
			auto case_expr = make_uniq<BoundCaseExpression>(expr->GetReturnType());
			BoundCaseCheck check;
			check.when_expr = std::move(gate);
			check.then_expr = make_uniq<BoundConstantExpression>(Value(expr->GetReturnType()));
			case_expr->CaseChecksMutable().push_back(std::move(check));
			case_expr->ElseMutable() = std::move(expr);
			expr = std::move(case_expr);
		}
	} else if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		// a bare column computed outside this node's select list (a group, a FROM or derived table
		// column, a flattened nested subquery): TRY has nothing to mask, errors there propagate
	} else {
		// a plain projection: push TRY onto the projected expression; TRY consumes the re-raises of
		// nested subqueries per row, and the HAS_ERROR probe observes them when the flag is exposed
		if (expr->IsVolatile()) {
			if (strict) {
				throw BinderException(location, "TRY can not be used in combination with a volatile function");
			}
			return optional<LogicalType>();
		}
		if (expose_error_flag) {
			auto probe = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_HAS_ERROR, LogicalType::BOOLEAN);
			probe->GetChildrenMutable().push_back(expr->Copy());
			error_flag = std::move(probe);
		}
		expr = WrapInTry(std::move(expr));
	}
	if (!expose_error_flag) {
		return optional<LogicalType>();
	}
	if (!error_flag) {
		error_flag = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
	}
	// pack the masked value and the error flag into a single struct column for the use site
	expr->SetAlias("value");
	error_flag->SetAlias("error");
	vector<pair<Identifier, unique_ptr<Expression>>> pack_children;
	pack_children.emplace_back(Identifier(), std::move(expr));
	pack_children.emplace_back(Identifier(), std::move(error_flag));
	auto &system_catalog = Catalog::GetSystemCatalog(context);
	auto &struct_pack = system_catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(system_catalog.GetName(), DEFAULT_SCHEMA, "struct_pack"));
	ErrorData error;
	FunctionBinder function_binder(context);
	auto packed = function_binder.BindScalarFunction(struct_pack, std::move(pack_children), error, false, &binder);
	if (!packed) {
		error.AddQueryLocation(location.GetQueryLocation());
		error.Throw();
	}
	auto struct_type = packed->GetReturnType();
	select_projection.expressions[item.binding.column_index] = std::move(packed);
	return struct_type;
}

optional<LogicalType> Binder::PlanTrySubquery(const BoundSubqueryExpression &expr, LogicalOperator &root,
                                              TrySubqueryLowering lowering) {
	auto &indexes = try_subquery_plan_indexes;
	D_ASSERT(IsUnderTry() && indexes.recorded);
	auto item = FindTrySubquerySelectItem(root, indexes);
	auto &select_projection = *item.projection;
	auto &item_expr = select_projection.expressions[item.binding.column_index];
	auto &item_root = select_projection.children[0];
	if (indexes.item_group.IsValid()) {
		// GROUP BY moved a scalar subquery item into this group, which takes the direct lowering like a bare item.
		// Any other lowering plans the group as it is, since a group is computed outside the maskable positions
		auto aggregate = FindAggregateOperator(*item_root, indexes.aggregate_index);
		if (!aggregate) {
			throw InternalException(
			    "TRY subquery: the aggregate node holding the group of the select item was not found");
		}
		auto &group = aggregate->groups[indexes.item_group];
		auto group_lowering =
		    lowering == TrySubqueryLowering::DIRECT ? TrySubqueryLowering::DIRECT : TrySubqueryLowering::NONE;
		group = PlanSubquery(group->Cast<BoundSubqueryExpression>(), aggregate->children[0], group_lowering);
	}
	// plan the subqueries of the item, which CreatePlan left for this point
	if (lowering == TrySubqueryLowering::DIRECT && item_expr->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY &&
	    item_expr->Cast<BoundSubqueryExpression>().CanLowerTry()) {
		// a select item that is itself a scalar subquery takes the direct lowering as well, which also covers
		// result modifiers inside it, and leaves nothing for TRY to mask in this node
		item_expr = PlanSubquery(item_expr->Cast<BoundSubqueryExpression>(), item_root, TrySubqueryLowering::DIRECT);
	} else {
		// the nested subqueries transport their error flag to the lowering of this node, if there is one
		auto item_lowering =
		    lowering == TrySubqueryLowering::NONE ? TrySubqueryLowering::NONE : TrySubqueryLowering::TRANSPORT;
		PlanSubqueries(item_expr, item_root, item_lowering);
	}
	if (lowering == TrySubqueryLowering::NONE) {
		return optional<LogicalType>();
	}
	return LowerTrySubquery(context, *this, expr, indexes, item, lowering);
}

} // namespace duckdb
