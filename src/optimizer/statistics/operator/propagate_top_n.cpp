#include "duckdb/common/sorting/topn_heap.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/optimizer/topn_optimizer.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"

namespace duckdb {

unique_ptr<NodeStatistics> StatisticsPropagator::PropagateStatistics(LogicalTopN &topn,
                                                                     unique_ptr<LogicalOperator> &node_ptr) {
	// first propagate to the child
	PropagateStatistics(topn.children[0]);

	// the Top-N was chosen before the estimate of the groups below it was tightened
	auto &child = *topn.children[0];
	if (!child.has_estimated_cardinality ||
	    TopNHeap::CanOptimize(topn.limit + topn.offset, child.estimated_cardinality) ||
	    !TopN::SortsGroupsByAggregateResult(topn)) {
		return nullptr;
	}
	// sorting all groups is faster for this estimate
	node_ptr = TopN::ToSortAndLimit(std::move(node_ptr));
	auto &order_ptr = node_ptr->children[0];
	for (auto &bound_order : order_ptr->Cast<LogicalOrder>().orders) {
		bound_order.stats = PropagateExpression(bound_order.expression);
	}
	order_ptr->ResolveOperatorTypesFromChildren();
	propagation_path.emplace_back(order_ptr);
	CompressOperator(order_ptr);
	propagation_path.pop_back();
	return nullptr;
}

} // namespace duckdb
