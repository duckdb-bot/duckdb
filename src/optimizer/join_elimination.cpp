#include "duckdb/optimizer/join_elimination.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/enums/logical_operator_type.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/optimizer/distinctness.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cte.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_unnest.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include <utility>

namespace duckdb {

//! Whether an expression of `op` itself is volatile
static bool AnyVolatile(const LogicalOperator &op) {
	bool is_volatile = false;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](const unique_ptr<Expression> *expr) {
		if ((*expr)->IsVolatile()) {
			is_volatile = true;
		}
	});
	return is_volatile;
}

RemovedSubtreeEffects::RemovedSubtreeEffects(LogicalOperator &plan) {
	CollectDefinitions(plan);
}

void RemovedSubtreeEffects::CollectDefinitions(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		materialized_ctes.emplace(op.Cast<LogicalCTE>().table_index, op);
	} else if (op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		recursive_ctes.emplace(op.Cast<LogicalCTE>().table_index, op);
	}
	for (auto &child : op.children) {
		CollectDefinitions(*child);
	}
}

bool RemovedSubtreeEffects::DefinitionHasEffect(TableIndex cte_index) {
	auto known = definition_has_effect.find(cte_index);
	if (known != definition_has_effect.end()) {
		return known->second;
	}
	auto cte = materialized_ctes.find(cte_index);
	if (cte == materialized_ctes.end()) {
		// a reference to something that is not defined in this plan: assume the worst
		return true;
	}
	// a materialized CTE cannot reference itself, so the definition is known before it is asked about again
	const bool has_effect = HasEffect(*cte->second.get().children[0]);
	definition_has_effect.emplace(cte_index, has_effect);
	return has_effect;
}

bool RemovedSubtreeEffects::HasEffect(LogicalOperator &subtree) {
	if (AnyVolatile(subtree)) {
		return true;
	}
	switch (subtree.type) {
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_DELETE:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
		return true;
	case LogicalOperatorType::LOGICAL_GET:
		if (subtree.Cast<LogicalGet>().function.stability == FunctionStability::VOLATILE) {
			return true;
		}
		break;
	case LogicalOperatorType::LOGICAL_CTE_REF: {
		auto cte_index = subtree.Cast<LogicalCTERef>().cte_index;
		if (recursive_ctes.find(cte_index) == recursive_ctes.end() && DefinitionHasEffect(cte_index)) {
			return true;
		}
		break;
	}
	default:
		break;
	}
	for (auto &child : subtree.children) {
		if (HasEffect(*child)) {
			return true;
		}
	}
	return false;
}

namespace {

//! The expressions and operators that compute the columns of a subtree
struct ColumnProducers {
	//! The expression that computes a column of a projection, an aggregate, a window or an unnest
	column_binding_map_t<reference<const Expression>> expressions;
	//! The columns of the children that a column of a set operation or of a recursive CTE returns
	column_binding_map_t<vector<ColumnBinding>> inputs;
	//! The CTE that a CTE reference reads
	unordered_map<TableIndex, TableIndex> cte_refs;
	//! The CTEs whose definition has been collected
	unordered_set<TableIndex> definitions;

	void AddExpressions(TableIndex table_index, const vector<unique_ptr<Expression>> &list) {
		for (idx_t i = 0; i < list.size(); i++) {
			expressions.emplace(ColumnBinding(table_index, ProjectionIndex(i)), *list[i]);
		}
	}

	//! A column of `op` returns the columns of its children at the same position, payload aggregates aside
	void AddInputs(TableIndex table_index, LogicalOperator &op) {
		for (auto &child : op.children) {
			auto child_bindings = child->GetColumnBindings();
			for (idx_t i = 0; i < child_bindings.size(); i++) {
				inputs[ColumnBinding(table_index, ProjectionIndex(i))].push_back(child_bindings[i]);
			}
		}
	}

	void Collect(LogicalOperator &op) {
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
			AddExpressions(op.Cast<LogicalProjection>().table_index, op.expressions);
			break;
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
			auto &aggr = op.Cast<LogicalAggregate>();
			AddExpressions(aggr.group_index, aggr.groups);
			AddExpressions(aggr.aggregate_index, aggr.expressions);
			break;
		}
		case LogicalOperatorType::LOGICAL_WINDOW:
			AddExpressions(op.Cast<LogicalWindow>().window_index, op.expressions);
			break;
		case LogicalOperatorType::LOGICAL_UNNEST:
			AddExpressions(op.Cast<LogicalUnnest>().unnest_index, op.expressions);
			break;
		case LogicalOperatorType::LOGICAL_UNION:
		case LogicalOperatorType::LOGICAL_EXCEPT:
		case LogicalOperatorType::LOGICAL_INTERSECT:
			AddInputs(op.Cast<LogicalSetOperation>().table_index, op);
			break;
		case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
			// a reference to the recursion reads these columns as well
			AddInputs(op.Cast<LogicalCTE>().table_index, op);
			definitions.insert(op.Cast<LogicalCTE>().table_index);
			break;
		case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
			definitions.insert(op.Cast<LogicalCTE>().table_index);
			break;
		case LogicalOperatorType::LOGICAL_CTE_REF: {
			auto &cte_ref = op.Cast<LogicalCTERef>();
			cte_refs.emplace(cte_ref.table_index, cte_ref.cte_index);
			break;
		}
		default:
			break;
		}
		for (auto &child : op.children) {
			Collect(*child);
		}
	}
};

} // namespace

bool RemovedSubtreeEffects::ReadsColumnWithEffect(LogicalOperator &reader, LogicalOperator &source) {
	vector<ColumnBinding> pending;
	LogicalOperatorVisitor::EnumerateExpressions(reader, [&](unique_ptr<Expression> *expr) {
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    **expr, [&](const BoundColumnRefExpression &ref) { pending.push_back(ref.Binding()); });
	});
	if (pending.empty()) {
		return false;
	}
	ColumnProducers producers;
	producers.Collect(source);
	// a column has an effect when its expression is volatile or reads a column that has one
	column_binding_set_t visited;
	while (!pending.empty()) {
		auto binding = pending.back();
		pending.pop_back();
		if (!visited.insert(binding).second) {
			continue;
		}
		auto cte_ref = producers.cte_refs.find(binding.table_index);
		if (cte_ref != producers.cte_refs.end()) {
			auto cte_index = cte_ref->second;
			auto recursion = recursive_ctes.find(cte_index);
			if (recursion != recursive_ctes.end()) {
				// the working table of a recursion holds the columns of the recursive CTE, which can be above the
				// source
				if (producers.definitions.insert(cte_index).second) {
					producers.Collect(recursion->second.get());
				}
				pending.emplace_back(cte_index, binding.column_index);
				continue;
			}
			auto cte = materialized_ctes.find(cte_index);
			if (cte == materialized_ctes.end()) {
				// a reference to something that is not defined in this plan counts as an effect
				return true;
			}
			// the column of the definition that the reference returns at this position
			auto &definition = *cte->second.get().children[0];
			if (producers.definitions.insert(cte_index).second) {
				producers.Collect(definition);
			}
			auto definition_bindings = definition.GetColumnBindings();
			if (binding.column_index.GetIndex() >= definition_bindings.size()) {
				return true;
			}
			pending.push_back(definition_bindings[binding.column_index.GetIndex()]);
			continue;
		}
		auto input = producers.inputs.find(binding);
		if (input != producers.inputs.end()) {
			pending.insert(pending.end(), input->second.begin(), input->second.end());
			continue;
		}
		auto expression = producers.expressions.find(binding);
		if (expression == producers.expressions.end()) {
			continue;
		}
		if (expression->second.get().IsVolatile()) {
			return true;
		}
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    expression->second.get(), [&](const BoundColumnRefExpression &ref) { pending.push_back(ref.Binding()); });
	}
	return false;
}

void JoinElimination::OptimizeChild(LogicalOperator &op, idx_t child_idx, JoinElimination &child_optimizer,
                                    bool replaceable) {
	// `op` may select the columns of its child by position: when a join is removed below, the selection is remapped
	auto projection_map = LogicalOperatorVisitor::GetProjectionMap(op, child_idx);
	const bool has_projection_map = projection_map && !projection_map->empty();
	vector<ColumnBinding> bindings_before;
	if (has_projection_map) {
		bindings_before = op.children[child_idx]->GetColumnBindings();
	}
	const auto removed_before = child_optimizer.removed_joins;
	child_optimizer.OptimizeOperator(op.children[child_idx], replaceable);
	if (has_projection_map && child_optimizer.removed_joins != removed_before) {
		LogicalOperatorVisitor::RemapProjectionMap(*projection_map, bindings_before,
		                                           op.children[child_idx]->GetColumnBindings());
	}
}

void JoinElimination::OptimizeJoin(unique_ptr<LogicalOperator> &op) {
	while (true) {
		auto &join = *op;
		// the pipelines of the two sides inherit the references of the operators above
		auto left_child = CreateChildren();
		auto right_child = CreateChildren();
		// the joins below must keep the columns this join's conditions read
		left_child->VisitOperatorExpressions(join);
		right_child->VisitOperatorExpressions(join);
		// repeated input rows only repeat the output rows of these joins, and a SEMI or ANTI join never sees how often
		// a row of its right side occurs. A volatile join condition is evaluated once per repeated row: it sees them
		const bool is_volatile = AnyVolatile(join);
		const bool ignores_duplicates = pipe_info.ignores_duplicates && !is_volatile;
		switch (join.Cast<LogicalComparisonJoin>().join_type) {
		case JoinType::INNER:
		case JoinType::LEFT:
		case JoinType::RIGHT:
		case JoinType::OUTER:
			left_child->pipe_info.ignores_duplicates = ignores_duplicates;
			right_child->pipe_info.ignores_duplicates = ignores_duplicates;
			break;
		case JoinType::SEMI:
		case JoinType::ANTI:
			left_child->pipe_info.ignores_duplicates = ignores_duplicates;
			right_child->pipe_info.ignores_duplicates = !is_volatile;
			break;
		case JoinType::MARK:
		case JoinType::SINGLE:
			// these return every row of their left side once; their right side is left alone
			left_child->pipe_info.ignores_duplicates = ignores_duplicates;
			break;
		default:
			break;
		}
		OptimizeJoinChild(join, 0, *left_child);
		OptimizeJoinChild(join, 1, *right_child);
		removed_joins += left_child->removed_joins + right_child->removed_joins;
		if (!TryEliminateJoin(op)) {
			return;
		}
		removed_joins++;
		// the outer side took the place of the join: when it is a join, it is tried in this pipeline in turn
		if (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
			return;
		}
	}
}

void JoinElimination::OptimizeJoinChild(LogicalOperator &join, idx_t child_idx, JoinElimination &child) {
	if (join.children[child_idx]->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		OptimizeChild(join, child_idx, child, true);
		return;
	}
	// a join that is the child of a join is a pipeline of its own that inherits the references and the
	// duplicate-insensitivity of this side: what is found below the nested join says nothing about its output
	auto nested = child.CreateChildren();
	nested->pipe_info.ignores_duplicates = child.pipe_info.ignores_duplicates;
	OptimizeChild(join, child_idx, *nested, true);
	child.removed_joins += nested->removed_joins;
}

void JoinElimination::OptimizeOperator(unique_ptr<LogicalOperator> &op_ptr, bool replaceable) {
	if (op_ptr->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		if (replaceable) {
			OptimizeJoin(op_ptr);
		}
		return;
	}
	// only the children of `op` are replaced from here on
	auto &op = *op_ptr;

	VisitOperatorExpressions(op);

	// whether the operators above return the same result when rows of the input of `op` are repeated
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		pipe_info.ignores_duplicates = op.Cast<LogicalAggregate>().IgnoresDuplicates();
		break;
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_UNNEST:
		// these return the rows of a repeated input row once per repeat; a volatile expression is evaluated once per
		// repeated row
		pipe_info.ignores_duplicates = pipe_info.ignores_duplicates && !AnyVolatile(op);
		break;
	default:
		// e.g. LIMIT, SAMPLE and windows count the repeated rows
		pipe_info.ignores_duplicates = false;
		break;
	}

	if (op.children.size() == 1) {
		OptimizeChild(op, 0, *this, true);
		return;
	}
	// every child starts a pipeline of its own; a child that is a join stays, its columns may be read by position
	for (idx_t child_idx = 0; child_idx < op.children.size(); child_idx++) {
		auto child_optimizer = CreateChildren();
		OptimizeChild(op, child_idx, *child_optimizer, false);
		removed_joins += child_optimizer->removed_joins;
	}
}

unique_ptr<LogicalOperator> JoinElimination::Optimize(unique_ptr<LogicalOperator> op) {
	effects = make_shared_ptr<RemovedSubtreeEffects>(*op);
	OptimizeOperator(op, false);
	return op;
}

bool JoinElimination::RemovedJoin() const {
	return removed_joins > 0;
}

bool JoinElimination::TryEliminateJoin(unique_ptr<LogicalOperator> &op) {
	auto &join = op->Cast<LogicalComparisonJoin>();
	idx_t inner_idx = 1;
	idx_t outer_idx = 0;
	switch (join.join_type) {
	case JoinType::LEFT:
	case JoinType::SINGLE:
		break;
	case JoinType::RIGHT:
		inner_idx = 0;
		outer_idx = 1;
		break;
	default:
		return false;
	}
	auto &outer_projection_map = outer_idx == 0 ? join.left_projection_map : join.right_projection_map;
	if (join.filter_pushdown) {
		return false;
	}
	auto inner_bindings = join.children[inner_idx]->GetColumnBindings();
	// ensure join output columns only contains outer table columns
	for (auto &binding : inner_bindings) {
		if (pipe_info.ref_table_ids.find(binding.table_index) != pipe_info.ref_table_ids.end()) {
			return false;
		}
	}
	// a join condition or an inner side with a volatile expression (nextval, random) stays, whatever is known about
	// the join: it has an effect of its own. So does an inner side that modifies data or that reads a materialized
	// CTE with such a definition
	if (AnyVolatile(join) || effects->HasEffect(*join.children[inner_idx])) {
		return false;
	}

	bool is_removable = false;
	if (pipe_info.ignores_duplicates && join.join_type != JoinType::SINGLE) {
		// a) the operators above ignore duplicates: an outer join that nothing reads the inner side of only repeats
		// its outer rows, whatever the inner side holds (a SINGLE join raises an error on a second match, so it is
		// left to the proof below)
		is_removable = true;
	} else {
		// b) the inner side is distinct on the columns its equality conditions compare: every outer row finds at
		// most one match, so the join returns exactly its outer rows. The other conditions only filter the matches,
		// and so does a filter on the inner side, pushed into a scan or not: it only removes inner rows.
		// The plan below this join is not rewritten any more, so the unique sets are inferred on a stable plan
		column_binding_set_t inner_keys;
		Distinctness distinctness(DistinctnessPropagation::UNARY_OPERATORS_AND_JOINS);
		is_removable = Distinctness::GetJoinEqualityKeys(join, inner_idx, inner_keys) &&
		               distinctness.IsDistinctOn(*join.children[inner_idx], inner_keys);
	}
	// a volatile column of the outer side that the join condition reads loses its reader with the join
	if (is_removable && effects->ReadsColumnWithEffect(join, *join.children[outer_idx])) {
		return false;
	}
	if (is_removable) {
		op = LogicalJoin::PromoteJoinSide(std::move(join.children[outer_idx]), std::move(outer_projection_map));
	}
	return is_removable;
}

unique_ptr<Expression> JoinElimination::VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) {
	pipe_info.ref_table_ids.insert(expr.Binding().table_index);
	return nullptr;
}

} // namespace duckdb
