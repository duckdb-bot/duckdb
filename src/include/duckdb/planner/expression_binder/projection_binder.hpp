//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/projection_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

class ColumnAliasBinder;

//! The Projection binder
class ProjectionBinder : public ExpressionBinder {
public:
	ProjectionBinder(Binder &binder, ClientContext &context, TableIndex proj_index,
	                 vector<unique_ptr<Expression>> &proj_expressions, string clause);

protected:
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
	                          bool root_expression = false) override;

	string UnsupportedAggregateMessage() override;

	//! Bind the expression and move it into the projection, leaving a reference to its column
	BindResult BindProjectedExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
	//! Move a bound expression into the projection and return the reference to its column
	unique_ptr<Expression> ProjectExpression(unique_ptr<Expression> expr);
	//! Move the column references and subqueries of a bound expression that stays in the action into the projection
	void ProjectLeaves(unique_ptr<Expression> &expr);

private:
	TableIndex proj_index;
	vector<unique_ptr<Expression>> &proj_expressions;
	string clause;
	bool in_child_projection = false;
};

} // namespace duckdb
