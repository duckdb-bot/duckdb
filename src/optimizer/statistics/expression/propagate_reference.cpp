#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(BoundReferenceExpression &ref,
                                                                     unique_ptr<Expression> &expr_ptr) {
	// references only have statistics within a lambda body, where they refer to the inputs of the lambda
	if (!lambda_input_stats || ref.Index() >= lambda_input_stats->size()) {
		return nullptr;
	}
	auto &input_stats = (*lambda_input_stats)[ref.Index()];
	return input_stats ? input_stats->ToUnique() : nullptr;
}

void StatisticsPropagator::PropagateLambdaStatistics(unique_ptr<Expression> &lambda_expr,
                                                     const vector<unique_ptr<BaseStatistics>> &input_stats) {
	// the inputs of an enclosing lambda body are restored afterwards, as nested lambdas have their own inputs
	auto enclosing_input_stats = lambda_input_stats;
	lambda_input_stats = &input_stats;
	PropagateExpression(lambda_expr);
	lambda_input_stats = enclosing_input_stats;
}

} // namespace duckdb
