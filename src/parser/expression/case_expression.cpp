#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"

namespace duckdb {

CaseExpression::CaseExpression() : ParsedExpression(ExpressionType::CASE_EXPR, ExpressionClass::CASE) {
}

bool CaseExpression::HasSimpleCaseChecks() const {
	if (!simple_case || case_checks.empty()) {
		return false;
	}
	for (auto &check : case_checks) {
		if (check.when_expr->GetExpressionClass() != ExpressionClass::COMPARISON ||
		    check.when_expr->GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
			return false;
		}
		if (!check.when_expr->Cast<ComparisonExpression>().Left().Equals(SimpleCaseOperand())) {
			return false;
		}
	}
	return true;
}

const ParsedExpression &CaseExpression::SimpleCaseOperand() const {
	D_ASSERT(simple_case && !case_checks.empty() &&
	         case_checks[0].when_expr->GetExpressionClass() == ExpressionClass::COMPARISON);
	return case_checks[0].when_expr->Cast<ComparisonExpression>().Left();
}

string CaseExpression::ToString() const {
	if (!HasSimpleCaseChecks()) {
		return ToString<CaseExpression, ParsedExpression>(*this);
	}
	string case_str = "CASE " + SimpleCaseOperand().ToString();
	for (auto &check : case_checks) {
		case_str += " WHEN " + check.when_expr->Cast<ComparisonExpression>().Right().ToString();
		case_str += " THEN " + check.then_expr->ToString();
	}
	case_str += " ELSE " + else_expr->ToString();
	case_str += " END";
	return case_str;
}

} // namespace duckdb
