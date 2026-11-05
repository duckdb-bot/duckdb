#include "duckdb/planner/expression_binder/projection_binder.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"

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
	bool rethrow_try_errors = false;
	if (result.expression->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		auto &subquery = result.expression->Cast<BoundSubqueryExpression>();
		if (subquery.RethrowsTryErrors()) {
			// the projection carries the packed value and error flag, and the error is re-raised here under the TRY
			subquery.ExposeTryErrorFlag();
			rethrow_try_errors = true;
		}
	}
	// we have successfully bound a column - push it into the projection and emit a reference
	auto return_type = result.expression->GetReturnType();
	auto alias = result.expression->GetName();
	auto proj_col_idx = ColumnBinding::PushExpression(proj_expressions, std::move(result.expression));
	auto proj_ref = make_uniq<BoundColumnRefExpression>(return_type, ColumnBinding(proj_index, proj_col_idx));
	proj_ref->SetAlias(std::move(alias));
	if (rethrow_try_errors) {
		return BindResult(binder.CreateTryErrorRethrow(std::move(proj_ref)));
	}
	return BindResult(std::move(proj_ref));
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
	case ExpressionClass::OPERATOR:
		if (IsTryOverScalarSubquery(expr.Cast<OperatorExpression>())) {
			// TRY directly over a scalar subquery is lowered onto its plan, so it moves into the projection whole
			return BindColumnRef(expr_ptr, depth, root_expression);
		}
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	default:
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
}

string ProjectionBinder::UnsupportedAggregateMessage() {
	return clause + " cannot contain aggregate functions";
}

} // namespace duckdb
