#include "duckdb/optimizer/filter_pullup.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

static void ReplaceFilterTableIndex(Expression &expr, LogicalSetOperation &setop) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		D_ASSERT(colref.Depth() == 0);

		colref.BindingMutable().table_index = setop.table_index;
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { ReplaceFilterTableIndex(child, setop); });
}

unique_ptr<LogicalOperator> FilterPullup::PullupIntersect(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_INTERSECT);
	auto &setop = op->Cast<LogicalSetOperation>();
	FilterPullup left_pullup(context, true, can_add_column);
	FilterPullup right_pullup(context, true, can_add_column);
	op->children[0] = left_pullup.Rewrite(std::move(op->children[0]));
	op->children[1] = right_pullup.Rewrite(std::move(op->children[1]));

	// INTERSECT returns rows of the left child, so a filter of the right child that tells apart rows INTERSECT treats
	// as equal stays there: above, it would select among the rows of the left child instead
	vector<unique_ptr<Expression>> right_filters;
	for (auto &expr : right_pullup.filters_expr_pullup) {
		if (setop.FilterTellsApartEqualRows(context, *expr)) {
			right_filters.push_back(std::move(expr));
		} else {
			left_pullup.filters_expr_pullup.push_back(std::move(expr));
		}
	}
	if (!right_filters.empty()) {
		op->children[1] = GeneratePullupFilter(std::move(op->children[1]), right_filters);
	}
	if (!left_pullup.filters_expr_pullup.empty()) {
		return GeneratePullupFilter(std::move(op), left_pullup.filters_expr_pullup);
	}
	return op;
}

unique_ptr<LogicalOperator> FilterPullup::PullupSetOperation(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_INTERSECT || op->type == LogicalOperatorType::LOGICAL_EXCEPT);
	can_add_column = false;
	can_pullup = true;
	if (op->type == LogicalOperatorType::LOGICAL_INTERSECT) {
		op = PullupIntersect(std::move(op));
	} else {
		// EXCEPT only pull ups from LHS
		op = PullupFromLeft(std::move(op));
	}
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op->Cast<LogicalFilter>();
		auto &setop = filter.children[0]->Cast<LogicalSetOperation>();
		for (idx_t i = 0; i < filter.expressions.size(); ++i) {
			ReplaceFilterTableIndex(*filter.expressions[i], setop);
		}
	}
	return op;
}

} // namespace duckdb
