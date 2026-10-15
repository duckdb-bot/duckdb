//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/not_comparison_simplification.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

// Rewrites NOT(comparison) into the negated comparison: NOT(a > b) -> a <= b, and NOT(a IS NULL) -> a IS NOT NULL
class NotComparisonSimplificationRule : public Rule {
public:
	explicit NotComparisonSimplificationRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

} // namespace duckdb
