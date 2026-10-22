#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"

namespace duckdb {

void ExpressionBinder::ReplaceMacroParametersInLambda(FunctionExpression &function,
                                                      vector<identifier_set_t> &lambda_params) {
	for (auto &child : function.GetArgumentsMutable()) {
		if (child.GetExpression().GetExpressionClass() != ExpressionClass::LAMBDA) {
			ReplaceMacroParameters(child.GetExpressionMutable(), lambda_params);
			continue;
		}

		// Special-handling for LHS lambda parameters.
		// We do not replace them, and we add them to the lambda_params vector.
		auto &lambda_expr = child.GetExpressionMutable()->Cast<LambdaExpression>();
		string error_message;
		auto column_ref_expressions = lambda_expr.ExtractColumnRefExpressions(error_message);

		if (!error_message.empty()) {
			// Possibly a JSON function, replace both LHS and RHS.
			ReplaceMacroParameters(lambda_expr.LeftMutable(), lambda_params);
			ReplaceMacroParameters(lambda_expr.RightMutable(), lambda_params);
			continue;
		}

		// Push the lambda parameter names of this level.
		lambda_params.emplace_back();
		for (const auto &column_ref_expr : column_ref_expressions) {
			const auto &column_ref = column_ref_expr.get().Cast<ColumnRefExpression>();
			lambda_params.back().emplace(column_ref.GetName());
		}

		// Only replace in the RHS of the expression.
		ReplaceMacroParameters(lambda_expr.RightMutable(), lambda_params);

		lambda_params.pop_back();
	}
}

void ExpressionBinder::ReplaceMacroParameters(unique_ptr<ParsedExpression> &expr,
                                              vector<identifier_set_t> &lambda_params,
                                              optional_ptr<vector<MacroParameterReference>> references) {
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		// If the expression is a column reference, we replace it with its argument.
		auto &col_ref = expr->Cast<ColumnRefExpression>();
		if (LambdaExpression::IsLambdaParameter(lambda_params, col_ref.GetName())) {
			return;
		}

		bool bind_macro_parameter = false;
		if (col_ref.IsQualified()) {
			// the table qualifier is the component directly before the column name
			auto &names = col_ref.ColumnNames();
			if (names[names.size() - 2].StartsWith(DummyBinding::DUMMY_NAME)) {
				bind_macro_parameter = true;
			}
		} else {
			bind_macro_parameter = macro_binding->HasMatchingBinding(col_ref.GetColumnName());
		}

		if (bind_macro_parameter) {
			D_ASSERT(macro_binding->HasMatchingBinding(col_ref.GetColumnName()));
			column_t argument_index;
			auto has_index = macro_binding->TryGetBindingIndex(col_ref.GetColumnName(), argument_index);
			expr = macro_binding->ParamToArg(col_ref);
			if (has_index && references) {
				references->push_back(MacroParameterReference {*expr, argument_index});
			}
		}
		return;
	}
	case ExpressionClass::FUNCTION: {
		// Special-handling for lambdas, which are inside function expressions.
		auto &function = expr->Cast<FunctionExpression>();
		if (function.IsLambdaFunction()) {
			// a reference in a lambda can refer to its parameters
			return ReplaceMacroParametersInLambda(function, lambda_params);
		}
		break;
	}
	case ExpressionClass::SUBQUERY: {
		auto &sq = (expr->Cast<SubqueryExpression>()).Subquery();
		// a reference in a subquery is bound in another scope
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    *sq->node, [&](unique_ptr<ParsedExpression> &child) { ReplaceMacroParameters(child, lambda_params); });
		break;
	}
	default:
		break;
	}

	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ReplaceMacroParameters(child, lambda_params, references); });
}

bool ExpressionBinder::CanBindMacroArgumentsOnce() const {
	return false;
}

bool ExpressionBinder::IsPureMacroArgument(const ParsedExpression &expr, const identifier_set_t &parameter_names,
                                           idx_t depth) {
	if (depth >= Settings::Get<MaxExpressionDepthSetting>(context)) {
		return false;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::CONSTANT:
		return true;
	case ExpressionClass::COLUMN_REF: {
		// only the parameters of a macro whose body is checked
		auto &col_ref = expr.Cast<ColumnRefExpression>();
		return !col_ref.IsQualified() && parameter_names.count(col_ref.GetColumnName()) > 0;
	}
	case ExpressionClass::OPERATOR:
		if (expr.GetExpressionType() == ExpressionType::GROUPING_FUNCTION) {
			return false;
		}
		break;
	case ExpressionClass::CAST:
	case ExpressionClass::COMPARISON:
	case ExpressionClass::CONJUNCTION:
	case ExpressionClass::CASE:
	case ExpressionClass::BETWEEN:
	case ExpressionClass::COLLATE:
		break;
	case ExpressionClass::FUNCTION: {
		auto &function = expr.Cast<FunctionExpression>();
		auto &name = function.GetQualifiedName();
		// a qualified name can turn into a column reference, see ColumnQualifier::QualifyFunction
		if (!name.Catalog().empty() || !name.Schema().empty() || function.Distinct() || function.Filter() ||
		    (function.OrderBy() && !function.OrderBy()->orders.empty()) || IsUnnestFunction(name.Name())) {
			return false;
		}
		EntryLookupInfo lookup(CatalogType::SCALAR_FUNCTION_ENTRY, name,
		                       QueryErrorContext(function.GetQueryLocation()));
		auto entry = GetCatalogEntry(lookup, OnEntryNotFound::RETURN_NULL);
		if (!entry) {
			return false;
		}
		if (entry->type == CatalogType::MACRO_ENTRY) {
			if (!IsPureMacro(*entry, depth + 1)) {
				return false;
			}
		} else if (entry->type != CatalogType::SCALAR_FUNCTION_ENTRY) {
			return false;
		}
		break;
	}
	default:
		// columns, parameters, subqueries, lambdas, windows, stars and the like
		return false;
	}
	bool result = true;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		result = result && IsPureMacroArgument(child, parameter_names, depth + 1);
	});
	return result;
}

static void CollectColumnNames(const ParsedExpression &expr, identifier_set_t &names) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		names.insert(expr.Cast<ColumnRefExpression>().GetColumnName());
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { CollectColumnNames(child, names); });
}

bool ExpressionBinder::IsPureMacro(CatalogEntry &macro, idx_t depth) {
	auto known = pure_macros.find(macro);
	if (known != pure_macros.end()) {
		return known->second;
	}
	// a macro that is still being checked, a recursive one, counts as not pure
	pure_macros[macro] = false;
	// with one overload, no defaults and every parameter in a pure body, binding a call binds all of the body and of
	// its arguments, so checking them looks up nothing that binding does not
	auto &functions = macro.Cast<ScalarMacroCatalogEntry>().macros;
	if (functions.size() != 1) {
		return false;
	}
	auto &scalar_macro = functions[0]->Cast<ScalarMacroFunction>();
	identifier_set_t parameter_names;
	for (auto &parameter : scalar_macro.parameters) {
		parameter_names.insert(parameter->Cast<ColumnRefExpression>().GetColumnName());
	}
	identifier_set_t referenced_names;
	CollectColumnNames(*scalar_macro.expression, referenced_names);
	auto result = scalar_macro.default_parameters.empty() &&
	              IsPureMacroArgument(*scalar_macro.expression, parameter_names, depth) &&
	              referenced_names.size() == parameter_names.size();
	pure_macros[macro] = result;
	return result;
}

// Find aggregate expression children
void ExpressionBinder::FindAggregateExprs(unique_ptr<ParsedExpression> &expr,
                                          vector<reference<unique_ptr<ParsedExpression>>> &exprs) {
	if (expr->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &fn_expr = expr->Cast<FunctionExpression>();

		// Look up the function in the catalog, check to see if it is actually an aggregate function
		EntryLookupInfo fn_entry(CatalogType::AGGREGATE_FUNCTION_ENTRY, fn_expr.GetQualifiedName());
		auto entry = GetCatalogEntry(fn_entry, OnEntryNotFound::RETURN_NULL);

		if (entry && entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
			exprs.push_back(expr);
			return;
		}
	}

	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child_expr) { FindAggregateExprs(child_expr, exprs); });
}

void ExpressionBinder::UnfoldWindowMacroExpression(unique_ptr<ParsedExpression> &expr, ScalarMacroFunction &macro_def) {
	auto macro_copy = macro_def.expression->Copy();
	vector<reference<unique_ptr<ParsedExpression>>> aggregate_exprs;
	FindAggregateExprs(macro_copy, aggregate_exprs);

	// Only allowed if the macro body has a single aggregate expression
	if (aggregate_exprs.size() != 1) {
		throw BinderException("Window function macro bodies must contain exactly one aggregate function");
	}

	// The window spec is pushed down to the aggregate function target within the macro body
	unique_ptr<ParsedExpression> &agg_expr_ref = aggregate_exprs[0];
	auto &agg_fn_expr = agg_expr_ref->Cast<FunctionExpression>();

	// Transfer the macro function attributes
	auto &window_expr = expr->Cast<WindowExpression>();
	window_expr.SetQualifiedName(agg_fn_expr.GetQualifiedName());
	window_expr.GetArgumentsMutable().clear();
	for (auto &arg : agg_fn_expr.GetArgumentsMutable()) {
		window_expr.GetArgumentsMutable().push_back(std::move(arg));
	}
	if (!window_expr.Distinct()) {
		window_expr.DistinctMutable() = agg_fn_expr.Distinct();
	}
	if (window_expr.Filter() && agg_fn_expr.Filter()) {
		// Two FILTER clauses: combine
		window_expr.FilterMutable() =
		    make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(window_expr.FilterMutable()),
		                                     std::move(agg_fn_expr.FilterMutable()));
	} else if (agg_fn_expr.Filter()) {
		//	One FILTER from the MACRO
		window_expr.FilterMutable() = std::move(agg_fn_expr.FilterMutable());
	}
	// Transfer argument ORDER BYs
	if (agg_fn_expr.OrderBy()) {
		auto clone = agg_fn_expr.OrderBy()->Copy();
		window_expr.ArgOrdersMutable() = std::move(clone->Cast<OrderModifier>().orders);
	}

	// Replace the aggregate expression with the new window expression
	agg_expr_ref = std::move(expr);
	expr = std::move(macro_copy);
}

void ExpressionBinder::UnfoldMacroExpression(FunctionExpression &function, ScalarMacroCatalogEntry &macro_func,
                                             unique_ptr<ParsedExpression> &expr, idx_t depth) {
	// validate the arguments and separate positional and default arguments
	vector<unique_ptr<ParsedExpression>> positional_arguments;
	InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> named_arguments;
	binder.lambda_bindings = lambda_bindings;
	auto bind_result = MacroFunction::BindMacroFunction(binder, macro_func.macros, macro_func.name, function,
	                                                    positional_arguments, named_arguments, depth);
	if (!bind_result.error.empty()) {
		throw BinderException(*expr, bind_result.error);
	}
	auto &macro_def = macro_func.macros[bind_result.function_idx.GetIndex()]->Cast<ScalarMacroFunction>();

	auto new_macro_binding =
	    MacroFunction::CreateDummyBinding(macro_def, macro_func.name, positional_arguments, named_arguments);
	macro_binding = new_macro_binding.get();

	// replace current expression with stored macro expression
	// special case: If this is a window function, then we need to return a window expression
	if (expr->GetExpressionClass() == ExpressionClass::WINDOW) {
		UnfoldWindowMacroExpression(expr, macro_def);
	} else {
		expr = macro_def.expression->Copy();
	}

	// qualify only the macro parameters with a new empty binder that only knows the macro binding
	auto dummy_binder = Binder::CreateBinder(context);
	dummy_binder->macro_binding = new_macro_binding.get();
	ExpressionBinder::QualifyColumnNames(*dummy_binder, expr);

	// now replace the parameters
	vector<identifier_set_t> lambda_params;
	// in a pure macro every reference is bound, and so is every argument
	if (!CanBindMacroArgumentsOnce() || !IsPureMacro(macro_func, 0)) {
		ReplaceMacroParameters(expr, lambda_params);
		return;
	}
	vector<MacroParameterReference> references;
	ReplaceMacroParameters(expr, lambda_params, references);
	// an argument that the macro reads several times is bound once, when it is pure
	auto &arguments = *new_macro_binding->arguments;
	vector<idx_t> reference_counts(arguments.size(), 0);
	for (auto &reference : references) {
		reference_counts[reference.argument_index]++;
	}
	vector<shared_ptr<MacroArgumentBinding>> argument_bindings(arguments.size());
	for (idx_t i = 0; i < arguments.size(); i++) {
		if (reference_counts[i] > 1 && IsPureMacroArgument(*arguments[i], identifier_set_t(), 0)) {
			argument_bindings[i] = make_shared_ptr<MacroArgumentBinding>(arguments[i]->Copy());
			argument_bindings[i]->unbound_references = reference_counts[i];
		}
	}
	for (auto &reference : references) {
		auto &argument_binding = argument_bindings[reference.argument_index];
		if (argument_binding) {
			macro_argument_references[reference.replacement.get()] = argument_binding;
		}
	}
}

BindResult ExpressionBinder::BindMacro(FunctionExpression &function, ScalarMacroCatalogEntry &macro_func, idx_t depth,
                                       unique_ptr<ParsedExpression> &expr) {
	auto stack_checker = StackCheck(*expr, 3);
	// expanding nested macros can take a long time, allow the query to be interrupted
	context.InterruptCheck();

	// unfold the macro expression
	UnfoldMacroExpression(function, macro_func, expr, depth);

	// bind the unfolded macro
	return BindExpression(expr, depth);
}

} // namespace duckdb
