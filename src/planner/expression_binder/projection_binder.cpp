#include "duckdb/planner/expression_binder/projection_binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

ProjectionBinder::ProjectionBinder(Binder &binder, ClientContext &context, TableIndex proj_index_p,
                                   vector<unique_ptr<Expression>> &proj_expressions_p, string clause_p)
    : ExpressionBinder(binder, context), proj_index(proj_index_p), proj_expressions(proj_expressions_p),
      clause(std::move(clause_p)) {
}

unique_ptr<Expression> ProjectionBinder::ProjectExpression(unique_ptr<Expression> expr) {
	auto return_type = expr->GetReturnType();
	auto alias = expr->GetName();
	auto proj_col_idx = ColumnBinding::PushExpression(proj_expressions, std::move(expr));
	auto proj_ref = make_uniq<BoundColumnRefExpression>(return_type, ColumnBinding(proj_index, proj_col_idx));
	proj_ref->SetAlias(std::move(alias));
	return std::move(proj_ref);
}

bool ProjectionBinder::ReferencesProjection(const Expression &expr) const {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		return expr.Cast<BoundColumnRefExpression>().Binding().table_index == proj_index;
	case ExpressionClass::BOUND_LAMBDA: {
		auto &lambda = expr.Cast<BoundLambdaExpression>();
		for (auto &capture : lambda.Captures()) {
			if (ReferencesProjection(*capture)) {
				return true;
			}
		}
		return ReferencesProjection(*lambda.LambdaExpr());
	}
	default: {
		bool result = false;
		ExpressionIterator::EnumerateChildren(
		    expr, [&](const Expression &child) { result = result || ReferencesProjection(child); });
		return result;
	}
	}
}

void ProjectionBinder::ProjectLeaves(unique_ptr<Expression> &expr) {
	auto type = expr->GetExpressionType();
	if ((type == ExpressionType::OPERATOR_TRY || type == ExpressionType::OPERATOR_HAS_ERROR) && expr->HasSubquery() &&
	    !ReferencesProjection(*expr)) {
		// the operator moves with its argument, so the subquery stays under the TRY that masks its errors
		expr = ProjectExpression(std::move(expr));
		return;
	}
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		if (expr->Cast<BoundColumnRefExpression>().Binding().table_index != proj_index) {
			expr = ProjectExpression(std::move(expr));
		}
		return;
	case ExpressionClass::BOUND_SUBQUERY:
		expr = ProjectExpression(std::move(expr));
		return;
	case ExpressionClass::BOUND_LAMBDA: {
		// the captured columns sit next to the body, which can hold further lambdas
		auto &lambda = expr->Cast<BoundLambdaExpression>();
		for (auto &capture : lambda.CapturesMutable()) {
			ProjectLeaves(capture);
		}
		ProjectLeaves(lambda.LambdaExprMutable());
		return;
	}
	default:
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { ProjectLeaves(child); });
		return;
	}
}

BindResult ProjectionBinder::BindProjectedExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	// bind the nested column references and subqueries in place, except in lambda bodies, which cannot hold a subquery
	const auto was_in_child_projection = in_child_projection;
	in_child_projection = true;
	auto result = ExpressionBinder::BindExpression(expr_ptr, depth);
	in_child_projection = was_in_child_projection;
	if (result.HasError()) {
		return result;
	}
	// a TRY or HAS_ERROR over a subquery moves with its argument, unless a lambda body in the argument moved its
	// subquery into the projection, which only the action can reference, and other expressions stay in the action
	// with their column references and subqueries projected
	ProjectLeaves(result.expression);
	return result;
}

BindResult ProjectionBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::DEFAULT:
		return BindUnsupportedExpression(expr, depth, clause + " cannot contain DEFAULT clause");
	case ExpressionClass::WINDOW:
		return BindUnsupportedExpression(expr, depth, clause + " cannot contain window functions!");
	case ExpressionClass::COLUMN_REF:
		if (in_child_projection) {
			return ExpressionBinder::BindExpression(expr_ptr, depth);
		}
		return BindProjectedExpression(expr_ptr, depth);
	case ExpressionClass::SUBQUERY:
		if (in_child_projection && !lambda_bindings) {
			// the subquery stays under the TRY that is projected with it
			return ExpressionBinder::BindExpression(expr_ptr, depth);
		}
		return BindProjectedExpression(expr_ptr, depth);
	case ExpressionClass::OPERATOR: {
		auto type = expr.GetExpressionType();
		if ((type == ExpressionType::OPERATOR_TRY || type == ExpressionType::OPERATOR_HAS_ERROR) &&
		    !in_child_projection && !lambda_bindings) {
			// the operator is bound with its argument, so a subquery in the argument can be projected under the TRY
			// that masks its errors. Inside a lambda body the argument can reference lambda parameters, which the
			// projection cannot see
			return BindProjectedExpression(expr_ptr, depth);
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
