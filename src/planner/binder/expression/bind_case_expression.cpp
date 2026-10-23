#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

//! The scalar function an unqualified call resolves to, if it resolves to one
static optional_ptr<ScalarFunctionCatalogEntry> TryGetScalarFunction(ExpressionBinder &binder,
                                                                     const FunctionExpression &function) {
	EntryLookupInfo lookup(CatalogType::SCALAR_FUNCTION_ENTRY, function.GetQualifiedName());
	auto entry = binder.GetBinder().GetCatalogEntry(lookup, OnEntryNotFound::RETURN_NULL);
	if (!entry || entry->type != CatalogType::SCALAR_FUNCTION_ENTRY) {
		return nullptr;
	}
	return entry->Cast<ScalarFunctionCatalogEntry>();
}

//! Whether a parsed expression holds what a lambda body cannot hold or what this cannot see into: a subquery, an
//! UNNEST, a macro call or a qualified function name
static bool ContainsUnsupported(ExpressionBinder &binder, const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return true;
	}
	optional_ptr<const QualifiedName> name;
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		name = expr.Cast<FunctionExpression>().GetQualifiedName();
	} else if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		// a macro can also be called as a window
		name = expr.Cast<WindowExpression>().GetQualifiedName();
	}
	if (name) {
		if (ExpressionBinder::IsUnnestFunction(name->Name()) || !name->Catalog().empty() || !name->Schema().empty()) {
			return true;
		}
		EntryLookupInfo lookup(CatalogType::SCALAR_FUNCTION_ENTRY, *name);
		auto entry = binder.GetBinder().GetCatalogEntry(lookup, OnEntryNotFound::RETURN_NULL);
		if (entry && entry->type == CatalogType::MACRO_ENTRY) {
			return true;
		}
	}
	bool result = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { result = result || ContainsUnsupported(binder, child); });
	return result;
}

//! Whether a parsed expression calls a scalar function with a volatile overload
static bool CallsVolatileFunction(ExpressionBinder &binder, const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto function = TryGetScalarFunction(binder, expr.Cast<FunctionExpression>());
		if (function) {
			for (auto &overload : function->functions.functions) {
				if (overload->GetStability() == FunctionStability::VOLATILE) {
					return true;
				}
			}
		}
	}
	bool result = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { result = result || CallsVolatileFunction(binder, child); });
	return result;
}

//! Whether a column reference of the expression may name an alias, which can hold what a lambda body cannot hold
static bool RefersToAlias(ExpressionBinder &binder, ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF &&
	    binder.ClaimsAlias(expr.Cast<ColumnRefExpression>())) {
		return true;
	}
	bool result = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](unique_ptr<ParsedExpression> &child) { result = result || RefersToAlias(binder, *child); });
	return result;
}

static void CollectColumnNames(const ParsedExpression &expr, case_insensitive_set_t &names) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		// every part, since the first one can name the parameter, as in parameter.field
		for (auto &part : expr.Cast<ColumnRefExpression>().ColumnNames()) {
			names.insert(part.GetIdentifierName());
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { CollectColumnNames(child, names); });
}

bool ExpressionBinder::CanEvaluateCaseOperandOnce(CaseExpression &expr) {
	if (!CanBindCaseOperandOnce() || expr.CaseChecks().size() < 2 || !expr.HasSimpleCaseChecks() ||
	    !CallsVolatileFunction(*this, expr.SimpleCaseOperand())) {
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
		if (ContainsUnsupported(*this, part.get())) {
			return false;
		}
	}
	for (auto &check : expr.CaseChecksMutable()) {
		if (RefersToAlias(*this, *check.when_expr->Cast<ComparisonExpression>().RightMutable()) ||
		    RefersToAlias(*this, *check.then_expr)) {
			return false;
		}
	}
	if (RefersToAlias(*this, *expr.ElseMutable())) {
		return false;
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
	if (CanEvaluateCaseOperandOnce(expr)) {
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
