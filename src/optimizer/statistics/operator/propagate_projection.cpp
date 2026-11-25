#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalProjection &proj,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	// first propagate to the child
	node_stats = PropagateStatistics(proj.children[0]);
	if (proj.children[0]->type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
		ReplaceWithEmptyResult(node_ptr);
		return std::move(node_stats);
	}
	// what an expression reads is judged as it is written, propagation can fold parts of it away
	vector<bool> can_be_folded;
	can_be_folded.reserve(proj.expressions.size());
	for (auto &expr : proj.expressions) {
		can_be_folded.push_back(CanBeFolded(*expr));
	}
	// then propagate to each of the expressions
	for (idx_t i = 0; i < proj.expressions.size(); i++) {
		auto stats = PropagateExpression(proj.expressions[i]);
		ColumnBinding binding(proj.table_index, ProjectionIndex(i));
		if (can_be_folded[i] && IsRemovable(*proj.expressions[i])) {
			removable_columns.insert(binding);
		}
		if (stats) {
			statistics_map.insert(make_pair(binding, std::move(stats)));
		}
	}
	return std::move(node_stats);
}

} // namespace duckdb
