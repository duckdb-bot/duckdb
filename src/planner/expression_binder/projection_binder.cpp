#include "duckdb/planner/expression_binder/projection_binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

ProjectionBinder::ProjectionBinder(Binder &binder, ClientContext &context, TableIndex proj_index_p,
                                   vector<unique_ptr<Expression>> &proj_expressions_p, string clause_p)
    : ExpressionBinder(binder, context), proj_index(proj_index_p), proj_expressions(proj_expressions_p),
      clause(std::move(clause_p)) {
}

BindResult ProjectionBinder::BindColumnRef(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	if (in_child_projection) {
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	in_child_projection = true;
	auto result = ExpressionBinder::BindExpression(expr_ptr, depth);
	in_child_projection = false;
	if (result.HasError()) {
		return result;
	}
	if (result.expression->GetExpressionClass() == ExpressionClass::BOUND_LAMBDA_REF) {
		return result;
	}
	// we have successfully bound a column - push it into the projection and emit a reference
	auto return_type = result.expression->GetReturnType();
	auto alias = result.expression->GetName();
	auto proj_col_idx = ColumnBinding::PushExpression(proj_expressions, std::move(result.expression));
	auto proj_ref = make_uniq<BoundColumnRefExpression>(return_type, ColumnBinding(proj_index, proj_col_idx));
	proj_ref->SetAlias(std::move(alias));
	return BindResult(std::move(proj_ref));
}

static bool ContainsLambda(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::LAMBDA) {
		return true;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr,
	                                            [&](const ParsedExpression &child) { found |= ContainsLambda(child); });
	return found;
}

BindResult ProjectionBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::DEFAULT:
		return BindUnsupportedExpression(expr, depth, clause + " cannot contain DEFAULT clause");
	case ExpressionClass::WINDOW:
		return BindUnsupportedExpression(expr, depth, clause + " cannot contain window functions!");
	case ExpressionClass::COLUMN_REF:
	case ExpressionClass::SUBQUERY:
		return BindColumnRef(expr_ptr, depth, root_expression);
	case ExpressionClass::OPERATOR: {
		auto type = expr.GetExpressionType();
		if ((type == ExpressionType::OPERATOR_TRY || type == ExpressionType::OPERATOR_HAS_ERROR) && !lambda_bindings &&
		    !ContainsLambda(expr)) {
			// a subquery under TRY, also one that a macro expands to, re-raises the errors it captured at its use
			// site, where TRY masks them, so the whole TRY moves into the projection with its subqueries. Lambdas
			// keep the old way, in which only the subquery moves, as a lambda parameter cannot leave its lambda.
			return BindColumnRef(expr_ptr, depth, root_expression);
		}
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
	default:
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
}

string ProjectionBinder::UnsupportedAggregateMessage() {
	return clause + " cannot contain aggregate functions";
}

} // namespace duckdb
