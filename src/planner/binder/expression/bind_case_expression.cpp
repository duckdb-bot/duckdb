#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

BindResult ExpressionBinder::BindCaseResult(const CaseExpression &expr, vector<BoundCaseCheck> &checks,
                                            unique_ptr<Expression> else_expr) {
	// figure out the result type of the CASE expression
	auto return_type = ExpressionBinder::GetExpressionReturnType(*else_expr);
	for (auto &check : checks) {
		auto then_type = ExpressionBinder::GetExpressionReturnType(*check.then_expr);
		if (!LogicalType::TryGetMaxLogicalType(context, return_type, then_type, return_type)) {
			throw BinderException(
			    expr, "Cannot mix values of type %s and %s in CASE expression - an explicit cast is required",
			    return_type.ToString(), then_type.ToString());
		}
	}

	// bind all the individual components of the CASE statement
	auto result = make_uniq<BoundCaseExpression>(return_type);
	for (auto &check : checks) {
		BoundCaseCheck result_check;
		result_check.when_expr =
		    BoundCastExpression::AddCastToType(context, std::move(check.when_expr), LogicalType::BOOLEAN);
		result_check.then_expr = BoundCastExpression::AddCastToType(context, std::move(check.then_expr), return_type);
		result->CaseChecksMutable().push_back(std::move(result_check));
	}
	result->ElseMutable() = BoundCastExpression::AddCastToType(context, std::move(else_expr), return_type);
	return BindResult(std::move(result));
}

//! Whether a parsed expression holds a subquery or UNNEST, which the body of a lambda cannot hold
static bool ContainsSubqueryOrUnnest(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION &&
	    ExpressionBinder::IsUnnestFunction(expr.Cast<FunctionExpression>().FunctionName())) {
		return true;
	}
	bool result = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { result = result || ContainsSubqueryOrUnnest(child); });
	return result;
}

static bool BranchesContainSubqueryOrUnnest(const CaseExpression &expr) {
	for (auto &check : expr.CaseChecks()) {
		if (ContainsSubqueryOrUnnest(check.when_expr->Cast<ComparisonExpression>().Right()) ||
		    ContainsSubqueryOrUnnest(*check.then_expr)) {
			return true;
		}
	}
	return ContainsSubqueryOrUnnest(expr.Else());
}

static bool ContainsSubquery(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		return true;
	}
	bool result = false;
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](const Expression &child) { result = result || ContainsSubquery(child); });
	return result;
}

BindResult ExpressionBinder::BindSimpleCase(CaseExpression &expr, idx_t depth) {
	// the operand is bound once, from a copy since the checks keep theirs
	ErrorData error;
	auto operand_expr = expr.SimpleCaseOperand().Copy();
	auto operand = BindChild(operand_expr, depth, error);
	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	if (expr.CaseChecks().size() > 1 && (operand->IsVolatile() || ContainsSubquery(*operand))) {
		// copies of the operand would each be evaluated
		return BindSimpleCaseOnce(expr, std::move(operand), depth);
	}
	vector<BoundCaseCheck> checks;
	for (auto &check : expr.CaseChecksMutable()) {
		auto &comparison = check.when_expr->Cast<ComparisonExpression>();
		auto value = BindChild(comparison.RightMutable(), depth, error);
		auto then_expr = BindChild(check.then_expr, depth, error);
		if (error.HasError()) {
			continue;
		}
		BoundCaseCheck bound_check;
		bound_check.when_expr =
		    CreateBoundComparison(ExpressionType::COMPARE_EQUAL, operand->Copy(), std::move(value), error);
		if (!bound_check.when_expr) {
			error.AddQueryLocation(comparison);
			return BindResult(std::move(error));
		}
		bound_check.then_expr = std::move(then_expr);
		checks.push_back(std::move(bound_check));
	}
	auto else_expr = BindChild(expr.ElseMutable(), depth, error);
	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	return BindCaseResult(expr, checks, std::move(else_expr));
}

BindResult ExpressionBinder::BindSimpleCaseOnce(CaseExpression &expr, unique_ptr<Expression> operand, idx_t depth) {
	// the CASE becomes the body of a lambda that invoke applies to the bound operand, which is evaluated once
	const string parameter = "#case_operand";
	auto body = make_uniq<CaseExpression>();
	for (auto &check : expr.CaseChecks()) {
		CaseCheck body_check;
		body_check.when_expr = make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(Identifier(parameter)),
		    check.when_expr->Cast<ComparisonExpression>().Right().Copy());
		body_check.then_expr = check.then_expr->Copy();
		body->CaseChecksMutable().push_back(std::move(body_check));
	}
	body->ElseMutable() = expr.Else().Copy();
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(make_uniq<LambdaExpression>(vector<string> {parameter}, std::move(body)));
	arguments.push_back(expr.SimpleCaseOperand().Copy());
	FunctionExpression invoke(Identifier("invoke"), std::move(arguments));
	invoke.SetQueryLocation(expr.GetQueryLocation());

	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &invoke_function = catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), "invoke"));
	vector<unique_ptr<Expression>> bound_arguments(2);
	bound_arguments[1] = std::move(operand);
	return BindLambdaFunction(invoke, invoke_function, depth, std::move(bound_arguments));
}

BindResult ExpressionBinder::BindExpression(CaseExpression &expr, idx_t depth) {
	// branches with a subquery or UNNEST keep the copies of the operand, since they cannot be evaluated in a lambda
	if (expr.HasSimpleCaseChecks() && !BranchesContainSubqueryOrUnnest(expr)) {
		return BindSimpleCase(expr, depth);
	}
	// first try to bind the children of the case expression
	ErrorData error;
	vector<BoundCaseCheck> checks;
	for (auto &check : expr.CaseChecksMutable()) {
		BoundCaseCheck bound_check;
		bound_check.when_expr = BindChild(check.when_expr, depth, error);
		bound_check.then_expr = BindChild(check.then_expr, depth, error);
		checks.push_back(std::move(bound_check));
	}
	auto else_expr = BindChild(expr.ElseMutable(), depth, error);
	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	return BindCaseResult(expr, checks, std::move(else_expr));
}
} // namespace duckdb
