#include "duckdb/planner/expression/bound_subquery_expression.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

BoundSubqueryExpression::BoundSubqueryExpression(LogicalType return_type)
    : Expression(ExpressionType::SUBQUERY, ExpressionClass::BOUND_SUBQUERY, std::move(return_type)) {
}

string BoundSubqueryExpression::ToString() const {
	return "SUBQUERY";
}

bool BoundSubqueryExpression::Equals(const BaseExpression &other_p) const {
	// equality between bound subqueries not implemented currently
	return false;
}

unique_ptr<Expression> BoundSubqueryExpression::Copy() const {
	throw SerializationException("Cannot copy BoundSubqueryExpression");
}

bool BoundSubqueryExpression::PropagatesNullValues() const {
	// TODO this can be optimized further by checking the actual subquery node
	return false;
}

bool BoundSubqueryExpression::IsVolatile() const {
	// the plan is evaluated once by its own operators, so it does not make the expression that reads its result
	// volatile. The positions TRY masks inside it are checked when TRY is pushed into the subquery
	return Expression::IsVolatile();
}

} // namespace duckdb
