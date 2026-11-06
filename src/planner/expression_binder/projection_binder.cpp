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

void ProjectionBinder::ProjectLeaves(unique_ptr<Expression> &expr) {
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
	const auto projected_count = proj_expressions.size();
	in_child_projection = true;
	auto result = ExpressionBinder::BindExpression(expr_ptr, depth);
	in_child_projection = was_in_child_projection;
	if (result.HasError()) {
		return result;
	}
	if (result.expression->GetExpressionClass() == ExpressionClass::BOUND_LAMBDA_REF) {
		return result;
	}
	if (proj_expressions.size() != projected_count) {
		// a lambda body moved its subqueries into the projection, which only the action can reference, so the
		// expression stays there and its remaining column references and subqueries move into the projection alone
		ProjectLeaves(result.expression);
		return result;
	}
	return BindResult(ProjectExpression(std::move(result.expression)));
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
			// the operator is projected together with its argument, so a subquery in the argument is planned under
			// the TRY that masks its errors. Inside a lambda body the argument can reference lambda parameters,
			// which the projection cannot see
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
