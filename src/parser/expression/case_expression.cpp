#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"

namespace duckdb {

CaseExpression::CaseExpression() : ParsedExpression(ExpressionType::CASE_EXPR, ExpressionClass::CASE) {
}

//! Whether the expression is a comparison a WHEN operand of a simple CASE can spell
static bool IsSimpleCaseComparison(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::COMPARISON) {
		return false;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return true;
	default:
		return false;
	}
}

//! The comparisons of a check, the check itself or the children of the OR that a WHEN list becomes, or none
template <class COMPARISON, class EXPRESSION>
static vector<reference<COMPARISON>> ComparisonsOfCheck(EXPRESSION &when_expr) {
	vector<reference<COMPARISON>> comparisons;
	if (IsSimpleCaseComparison(when_expr)) {
		comparisons.push_back(when_expr.template Cast<ComparisonExpression>());
		return comparisons;
	}
	if (when_expr.GetExpressionType() != ExpressionType::CONJUNCTION_OR) {
		return comparisons;
	}
	for (auto &child : when_expr.template Cast<ConjunctionExpression>().GetChildren()) {
		if (!IsSimpleCaseComparison(*child)) {
			comparisons.clear();
			return comparisons;
		}
		comparisons.push_back(child->template Cast<ComparisonExpression>());
	}
	return comparisons;
}

vector<reference<const ComparisonExpression>> CaseExpression::SimpleCaseComparisons(const ParsedExpression &when_expr) {
	return ComparisonsOfCheck<const ComparisonExpression>(when_expr);
}

vector<reference<ComparisonExpression>> CaseExpression::SimpleCaseComparisonsMutable(ParsedExpression &when_expr) {
	return ComparisonsOfCheck<ComparisonExpression>(when_expr);
}

bool CaseExpression::HasSimpleCaseChecks() const {
	if (!simple_case || case_checks.empty() || SimpleCaseComparisons(*case_checks[0].when_expr).empty()) {
		return false;
	}
	auto &operand = SimpleCaseOperand();
	for (auto &check : case_checks) {
		auto comparisons = SimpleCaseComparisons(*check.when_expr);
		if (comparisons.empty()) {
			return false;
		}
		for (auto &comparison : comparisons) {
			if (!comparison.get().Left().Equals(operand)) {
				return false;
			}
		}
	}
	return true;
}

const ParsedExpression &CaseExpression::SimpleCaseOperand() const {
	D_ASSERT(simple_case && !case_checks.empty());
	auto comparisons = SimpleCaseComparisons(*case_checks[0].when_expr);
	D_ASSERT(!comparisons.empty());
	return comparisons[0].get().Left();
}

string CaseExpression::ToString() const {
	if (!HasSimpleCaseChecks()) {
		return ToString<CaseExpression, ParsedExpression>(*this);
	}
	string case_str = "CASE " + SimpleCaseOperand().ToString();
	for (auto &check : case_checks) {
		case_str += " WHEN ";
		auto comparisons = SimpleCaseComparisons(*check.when_expr);
		for (idx_t i = 0; i < comparisons.size(); i++) {
			auto &comparison = comparisons[i].get();
			if (i > 0) {
				case_str += ", ";
			}
			// a WHEN operand without an operator compares with =
			if (comparison.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
				case_str += ExpressionTypeToOperator(comparison.GetExpressionType()) + " ";
			}
			case_str += comparison.Right().ToString();
		}
		case_str += " THEN " + check.then_expr->ToString();
	}
	case_str += " ELSE " + else_expr->ToString();
	case_str += " END";
	return case_str;
}

} // namespace duckdb
