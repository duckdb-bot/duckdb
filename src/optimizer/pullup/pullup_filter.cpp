#include "duckdb/optimizer/filter_pullup.hpp"
#include "duckdb/planner/expression/legacy_bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> FilterPullup::PullupFilter(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_FILTER);

	auto &filter = op->Cast<LogicalFilter>();
	if (can_pullup && !filter.HasProjectionMap()) {
		const auto pulled_before_child = filters_expr_pullup.size();
		unique_ptr<LogicalOperator> child = std::move(op->children[0]);
		child = Rewrite(std::move(child));
		// moving filter's expressions, a volatile one stays where it runs once per row of the input
		auto volatile_expressions = PullupNonVolatile(op->expressions);
		if (volatile_expressions.empty()) {
			return child;
		}
		for (auto &expr : volatile_expressions) {
			if (expr->IsVolatile() && expr->CanThrow()) {
				// the guards pulled up from below a volatile expression that can throw stay below it
				vector<unique_ptr<Expression>> guards;
				for (idx_t i = pulled_before_child; i < filters_expr_pullup.size(); i++) {
					guards.push_back(std::move(filters_expr_pullup[i]));
				}
				filters_expr_pullup.erase(filters_expr_pullup.begin() + NumericCast<int64_t>(pulled_before_child),
				                          filters_expr_pullup.end());
				if (!guards.empty()) {
					child = GeneratePullupFilter(std::move(child), guards);
				}
				break;
			}
		}
		return GeneratePullupFilter(std::move(child), volatile_expressions);
	}
	op->children[0] = Rewrite(std::move(op->children[0]));
	return op;
}

} // namespace duckdb
