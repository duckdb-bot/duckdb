#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

//! Whether a parsed expression holds a subquery, an UNNEST or a macro call, which a lambda body cannot hold or which
//! would be bound with effects
static bool ContainsSubqueryUnnestOrMacro(ExpressionBinder &binder, const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &function = expr.Cast<FunctionExpression>();
		if (ExpressionBinder::IsUnnestFunction(function.FunctionName())) {
			return true;
		}
		auto &name = function.GetQualifiedName();
		if (!name.Catalog().empty() || !name.Schema().empty()) {
			// a qualified name may also be a macro, or a call on a column
			return true;
		}
		EntryLookupInfo lookup(CatalogType::SCALAR_FUNCTION_ENTRY, name);
		auto entry = binder.GetBinder().GetCatalogEntry(lookup, OnEntryNotFound::RETURN_NULL);
		if (entry && entry->type == CatalogType::MACRO_ENTRY) {
			return true;
		}
	}
	bool result = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { result = result || ContainsSubqueryUnnestOrMacro(binder, child); });
	return result;
}

static void CollectColumnNames(const ParsedExpression &expr, case_insensitive_set_t &names) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		names.insert(expr.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName());
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { CollectColumnNames(child, names); });
}

bool ExpressionBinder::CanEvaluateCaseOperandOnce(CaseExpression &expr, idx_t depth) {
	if (!CanBindCaseOperandOnce() || expr.CaseChecks().size() < 2 || !expr.HasSimpleCaseChecks()) {
		return false;
	}
	vector<reference<const ParsedExpression>> parts;
	parts.push_back(expr.SimpleCaseOperand());
	for (auto &check : expr.CaseChecks()) {
		parts.push_back(check.when_expr->Cast<ComparisonExpression>().Right());
		parts.push_back(*check.then_expr);
	}
	parts.push_back(expr.Else());
	for (auto &part : parts) {
		if (ContainsSubqueryUnnestOrMacro(*this, part.get())) {
			return false;
		}
	}
	// a binder without a select node registers nothing, it tells whether the operand is volatile and whether a lambda
	// body can hold every branch (it refuses aliases, aggregates and windows as well, which keep the copies)
	ExpressionBinder probe(binder, context);
	probe.lambda_bindings = lambda_bindings;
	probe.inaccessible_lambda_binding_count = inaccessible_lambda_binding_count;
	for (idx_t i = 0; i < parts.size(); i++) {
		auto part = parts[i].get().Copy();
		auto result = probe.Bind(part, depth);
		if (result.HasError() || (i == 0 && !result.expression->IsVolatile())) {
			return false;
		}
	}
	return true;
}

BindResult ExpressionBinder::BindCaseOperandOnce(CaseExpression &expr, idx_t depth) {
	// the parameter has a name that no branch refers to, so that it captures nothing
	case_insensitive_set_t names;
	for (auto &check : expr.CaseChecks()) {
		CollectColumnNames(check.when_expr->Cast<ComparisonExpression>().Right(), names);
		CollectColumnNames(*check.then_expr, names);
	}
	CollectColumnNames(expr.Else(), names);
	string parameter = "#case_operand";
	for (idx_t i = 1; names.count(parameter) > 0; i++) {
		parameter = "#case_operand_" + to_string(i);
	}

	auto body = make_uniq<CaseExpression>();
	body->SetQueryLocation(expr.GetQueryLocation());
	for (auto &check : expr.CaseChecks()) {
		auto &comparison = check.when_expr->Cast<ComparisonExpression>();
		CaseCheck body_check;
		body_check.when_expr = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
		                                                       make_uniq<ColumnRefExpression>(Identifier(parameter)),
		                                                       comparison.Right().Copy());
		body_check.when_expr->SetQueryLocation(comparison.GetQueryLocation());
		body_check.then_expr = check.then_expr->Copy();
		body->CaseChecksMutable().push_back(std::move(body_check));
	}
	body->ElseMutable() = expr.Else().Copy();
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(make_uniq<LambdaExpression>(vector<string> {parameter}, std::move(body)));
	arguments.push_back(expr.SimpleCaseOperand().Copy());
	// the system function, which a macro of the same name cannot hide
	unique_ptr<ParsedExpression> invoke = make_uniq<FunctionExpression>(
	    QualifiedName(Identifier(SYSTEM_CATALOG), Identifier(DEFAULT_SCHEMA), Identifier("invoke")),
	    std::move(arguments));
	invoke->SetQueryLocation(expr.GetQueryLocation());
	return Bind(invoke, depth);
}

BindResult ExpressionBinder::BindExpression(CaseExpression &expr, idx_t depth) {
	// copies of a volatile operand would each be evaluated, so it is evaluated once as the argument of a lambda
	if (CanEvaluateCaseOperandOnce(expr, depth)) {
		return BindCaseOperandOnce(expr, depth);
	}
	// first try to bind the children of the case expression
	ErrorData error;
	vector<pair<unique_ptr<Expression>, unique_ptr<Expression>>> checks;
	for (auto &check : expr.CaseChecksMutable()) {
		auto when_expr = BindChild(check.when_expr, depth, error);
		auto then_expr = BindChild(check.then_expr, depth, error);
		checks.emplace_back(std::move(when_expr), std::move(then_expr));
	}
	auto else_expr = BindChild(expr.ElseMutable(), depth, error);
	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	// the children have been successfully resolved
	// figure out the result type of the CASE expression
	auto return_type = ExpressionBinder::GetExpressionReturnType(*else_expr);
	for (auto &check : checks) {
		auto then_type = ExpressionBinder::GetExpressionReturnType(*check.second);
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
		    BoundCastExpression::AddCastToType(context, std::move(check.first), LogicalType::BOOLEAN);
		result_check.then_expr = BoundCastExpression::AddCastToType(context, std::move(check.second), return_type);
		result->CaseChecksMutable().push_back(std::move(result_check));
	}
	result->ElseMutable() = BoundCastExpression::AddCastToType(context, std::move(else_expr), return_type);
	return BindResult(std::move(result));
}
} // namespace duckdb
