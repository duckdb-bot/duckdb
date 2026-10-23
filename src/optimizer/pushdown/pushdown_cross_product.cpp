#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/in_clause_rewriter.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"

namespace duckdb {

using Filter = FilterPushdown::Filter;

static bool HasTransitiveJoinBounds(const vector<unique_ptr<Filter>> &filters) {
	unordered_set<TableIndex> bound_tables;
	for (auto &filter : filters) {
		if (filter->bindings.size() != 1) {
			continue;
		}
		auto &expr = *filter->filter;
		bool has_bound = false;
		if (BoundComparisonExpression::IsComparison(expr)) {
			auto &comparison = expr.Cast<BoundFunctionExpression>();
			has_bound = BoundComparisonExpression::Left(comparison).IsFoldable() ||
			            BoundComparisonExpression::Right(comparison).IsFoldable();
		} else if (expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			auto &between = expr.Cast<BoundFunctionExpression>();
			has_bound = BoundBetweenExpression::LowerBound(between).IsFoldable() ||
			            BoundBetweenExpression::UpperBound(between).IsFoldable();
		}
		if (has_bound) {
			bound_tables.insert(*filter->bindings.begin());
		}
	}
	if (bound_tables.empty()) {
		return false;
	}
	for (auto &filter : filters) {
		if (filter->bindings.size() < 2 || !BoundComparisonExpression::IsComparison(*filter->filter)) {
			continue;
		}
		auto &comparison = filter->filter->Cast<BoundFunctionExpression>();
		auto type = comparison.GetExpressionType();
		if (type != ExpressionType::COMPARE_GREATERTHAN && type != ExpressionType::COMPARE_GREATERTHANOREQUALTO &&
		    type != ExpressionType::COMPARE_LESSTHAN && type != ExpressionType::COMPARE_LESSTHANOREQUALTO) {
			continue;
		}
		auto &right = BoundComparisonExpression::Right(comparison);
		if (right.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF &&
		    bound_tables.count(right.Cast<BoundColumnRefExpression>().Binding().table_index)) {
			return true;
		}
	}
	return false;
}

static optional_ptr<const BoundColumnRefExpression> AsColumnRef(const Expression &expr) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return nullptr;
	}
	return &expr.Cast<BoundColumnRefExpression>();
}

static JoinSide GetColumnSide(const BoundColumnRefExpression &column, const unordered_set<TableIndex> &left_bindings,
                              const unordered_set<TableIndex> &right_bindings) {
	return JoinSide::GetJoinSide(column.Binding().table_index, left_bindings, right_bindings);
}

static bool IsUsed(const vector<reference<const BoundColumnRefExpression>> &used,
                   const BoundColumnRefExpression &column) {
	for (auto &entry : used) {
		if (entry.get().Equals(column)) {
			return true;
		}
	}
	return false;
}

//! A row that passes `l IS NOT DISTINCT FROM r` and `l = c` has a non-NULL l, so it also passes `r = c`. With r and c
//! on the same side of the join, that equality filters the side before the join. Each column is in one such filter at
//! most, as the side combines its equalities with each other.
static vector<unique_ptr<Expression>> GetImpliedSideFilters(const vector<unique_ptr<Expression>> &join_expressions,
                                                            const unordered_set<TableIndex> &left_bindings,
                                                            const unordered_set<TableIndex> &right_bindings) {
	vector<unique_ptr<Expression>> result;
	vector<reference<const BoundColumnRefExpression>> used;
	for (auto &null_safe : join_expressions) {
		if (null_safe->GetExpressionType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM ||
		    !BoundComparisonExpression::IsComparison(*null_safe)) {
			continue;
		}
		auto &null_safe_comparison = null_safe->Cast<BoundFunctionExpression>();
		auto null_safe_left = AsColumnRef(BoundComparisonExpression::Left(null_safe_comparison));
		auto null_safe_right = AsColumnRef(BoundComparisonExpression::Right(null_safe_comparison));
		if (!null_safe_left || !null_safe_right) {
			continue;
		}
		for (auto &equality : join_expressions) {
			if (equality->GetExpressionType() != ExpressionType::COMPARE_EQUAL ||
			    !BoundComparisonExpression::IsComparison(*equality)) {
				continue;
			}
			auto &equality_comparison = equality->Cast<BoundFunctionExpression>();
			auto equality_left = AsColumnRef(BoundComparisonExpression::Left(equality_comparison));
			auto equality_right = AsColumnRef(BoundComparisonExpression::Right(equality_comparison));
			if (!equality_left || !equality_right) {
				continue;
			}
			reference<const BoundColumnRefExpression> null_safe_sides[] = {*null_safe_left, *null_safe_right};
			reference<const BoundColumnRefExpression> equality_sides[] = {*equality_left, *equality_right};
			for (idx_t n = 0; n < 2; n++) {
				auto &shared = null_safe_sides[n].get();
				auto &other = null_safe_sides[1 - n].get();
				for (idx_t e = 0; e < 2; e++) {
					auto &partner = equality_sides[1 - e].get();
					if (!equality_sides[e].get().Equals(shared) || partner.Equals(other)) {
						continue;
					}
					auto side = GetColumnSide(other, left_bindings, right_bindings);
					if ((side != JoinSide::LEFT && side != JoinSide::RIGHT) ||
					    GetColumnSide(partner, left_bindings, right_bindings) != side) {
						continue;
					}
					if (IsUsed(used, other) || IsUsed(used, partner)) {
						continue;
					}
					used.push_back(other);
					used.push_back(partner);
					result.push_back(
					    BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, other.Copy(), partner.Copy()));
				}
			}
		}
	}
	return result;
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownCrossProduct(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->children.size() > 1);
	FilterPushdown left_pushdown(optimizer, convert_mark_joins, projection_mode);
	FilterPushdown right_pushdown(optimizer, convert_mark_joins, projection_mode);
	vector<unique_ptr<Expression>> join_expressions;
	vector<unique_ptr<Expression>> deferred_filters;
	auto join_ref_type = JoinRefType::REGULAR;
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		break;
	default:
		throw InternalException("Unsupported join type for cross product push down");
	}
	unordered_set<TableIndex> left_bindings, right_bindings;
	if (!filters.empty()) {
		// Recombine useful join bounds before distributing filters to the inputs.
		if (HasTransitiveJoinBounds(filters)) {
			if (PushFilters() == FilterResult::UNSATISFIABLE) {
				return make_uniq<LogicalEmptyResult>(std::move(op));
			}
			GenerateFilters();
		}
		// check to see into which side we should push the filters
		// first get the LHS and RHS bindings
		LogicalJoin::GetTableReferences(*op->children[0], left_bindings);
		LogicalJoin::GetTableReferences(*op->children[1], right_bindings);
		// now check the set of filters
		for (auto &f : filters) {
			auto side = JoinSide::GetJoinSide(f->bindings, left_bindings, right_bindings);
			if (side == JoinSide::LEFT) {
				// bindings match left side: push into left
				left_pushdown.filters.push_back(std::move(f));
			} else if (side == JoinSide::RIGHT) {
				right_pushdown.filters.push_back(std::move(f));
			} else {
				D_ASSERT(side == JoinSide::BOTH || side == JoinSide::NONE);
				if (InClauseRewriter::HasRewritableInClause(*f->filter)) {
					deferred_filters.push_back(std::move(f->filter));
				} else {
					// bindings match both: turn into join condition
					join_expressions.push_back(std::move(f->filter));
				}
			}
		}
		for (auto &implied : GetImpliedSideFilters(join_expressions, left_bindings, right_bindings)) {
			auto f = make_uniq<Filter>(std::move(implied));
			f->ExtractBindings();
			auto side = JoinSide::GetJoinSide(f->bindings, left_bindings, right_bindings);
			if (side == JoinSide::LEFT) {
				left_pushdown.filters.push_back(std::move(f));
			} else {
				D_ASSERT(side == JoinSide::RIGHT);
				right_pushdown.filters.push_back(std::move(f));
			}
		}
	}

	op->children[0] = left_pushdown.Rewrite(std::move(op->children[0]));
	op->children[1] = right_pushdown.Rewrite(std::move(op->children[1]));

	unique_ptr<LogicalOperator> result;
	if (!join_expressions.empty()) {
		// join conditions found: turn into inner join
		// extract join conditions
		vector<JoinCondition> conditions;
		const auto join_type = JoinType::INNER;
		LogicalComparisonJoin::ExtractJoinConditions(GetContext(), join_type, join_ref_type, op->children[0],
		                                             op->children[1], left_bindings, right_bindings, join_expressions,
		                                             conditions);
		// create the join from the join conditions
		result = LogicalComparisonJoin::CreateJoin(join_type, join_ref_type, std::move(op->children[0]),
		                                           std::move(op->children[1]), std::move(conditions));

		// possible cases are: AnyJoin, ComparisonJoin, or Filter + ComparisonJoin
		if (op->has_estimated_cardinality) {
			// set the estimated cardinality of the new operator
			result->SetEstimatedCardinality(op->estimated_cardinality);
			if (result->type == LogicalOperatorType::LOGICAL_FILTER) {
				// if the new operators are Filter + ComparisonJoin, also set the estimated cardinality for the join
				D_ASSERT(result->children[0]->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN);
				result->children[0]->SetEstimatedCardinality(op->estimated_cardinality);
			}
		}
	} else {
		// no join conditions found: keep as cross product
		D_ASSERT(op->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
		result = std::move(op);
	}
	if (deferred_filters.empty()) {
		return result;
	}
	auto filter = make_uniq<LogicalFilter>();
	filter->expressions = std::move(deferred_filters);
	filter->children.push_back(std::move(result));
	if (filter->children[0]->has_estimated_cardinality) {
		filter->SetEstimatedCardinality(filter->children[0]->estimated_cardinality);
	}
	return std::move(filter);
}

} // namespace duckdb
