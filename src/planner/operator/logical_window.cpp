#include "duckdb/planner/operator/logical_window.hpp"

#include "duckdb/function/window_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

vector<ColumnBinding> LogicalWindow::GetColumnBindings() {
	auto child_bindings = children[0]->GetColumnBindings();
	for (auto window_col_idx : ProjectionIndex::GetIndexes(expressions.size())) {
		child_bindings.emplace_back(window_index, window_col_idx);
	}
	return child_bindings;
}

void LogicalWindow::ResolveTypes() {
	types.insert(types.end(), children[0]->types.begin(), children[0]->types.end());
	for (auto &expr : expressions) {
		types.push_back(expr->GetReturnType());
	}
}

vector<TableIndex> LogicalWindow::GetTableIndex() const {
	return vector<TableIndex> {window_index};
}

static bool IsPartition(const Expression &partition, const Expression &expr) {
	if (partition.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	    expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return partition.Cast<BoundColumnRefExpression>().Binding() == expr.Cast<BoundColumnRefExpression>().Binding();
	}
	return partition.Equals(expr);
}

bool LogicalWindow::ComparesPartitions(const Expression &filter) const {
	auto compared_child = BoundComparisonExpression::ConstantComparisonChild(filter);
	if (!compared_child.IsValid() || expressions.empty()) {
		return false;
	}
	optional_ptr<const Expression> compared;
	idx_t child_idx = 0;
	ExpressionIterator::EnumerateChildren(filter, [&](const Expression &child) {
		if (child_idx++ == compared_child.GetIndex()) {
			compared = &child;
		}
	});
	for (auto &expr : expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_WINDOW) {
			return false;
		}
		bool partitions_by_compared = false;
		for (auto &partition : expr->Cast<BoundWindowExpression>().Partitions()) {
			partitions_by_compared = partitions_by_compared || IsPartition(*partition, *compared);
		}
		if (!partitions_by_compared) {
			return false;
		}
	}
	return true;
}

bool LogicalWindow::OnlyRanksWithoutArguments() const {
	for (auto &expr : expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_WINDOW || expr->IsVolatile() || expr->CanThrow()) {
			return false;
		}
		auto &window_expr = expr->Cast<BoundWindowExpression>();
		if (!window_expr.WindowFunction() || !window_expr.WindowFunction()->IsRanking() ||
		    window_expr.AggregateFunction() || !window_expr.GetChildren().empty() || window_expr.Filter() ||
		    window_expr.StartExpr() || window_expr.EndExpr()) {
			return false;
		}
	}
	return true;
}

string LogicalWindow::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return LogicalOperator::GetName() + StringUtil::Format(" #%llu", window_index.index);
	}
#endif
	return LogicalOperator::GetName();
}

} // namespace duckdb
