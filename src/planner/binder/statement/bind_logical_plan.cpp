#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include <algorithm>

namespace duckdb {

TableIndex GetMaxTableIndex(LogicalOperator &op) {
	TableIndex result(0);
	for (auto &child : op.children) {
		auto max_child_index = GetMaxTableIndex(*child);
		result = MaxValue<TableIndex>(result, max_child_index);
	}
	auto indexes = op.GetTableIndex();
	for (auto &index : indexes) {
		result = MaxValue<TableIndex>(result, index);
	}
	return result;
}

namespace {

//! A plan serialized before an aggregate's or a window function's return type changed gets that function wrapped in
//! a cast to the serialized type, which BoundAggregateExpression::Deserialize and BoundWindowExpression::Deserialize
//! put there. A LogicalAggregate may only hold aggregates and a LogicalWindow only window expressions, so the function
//! keeps its type and a projection directly above the operator casts it back, so every operator above it, including
//! those that take its columns without an expression (set operations, CTEs), sees the types the plan was written with
void RestoreSerializedFunctionTypes(ClientContext &context, unique_ptr<LogicalOperator> &root,
                                    unique_ptr<LogicalOperator> &op, TableIndex &next_table_index) {
	for (auto &child : op->children) {
		RestoreSerializedFunctionTypes(context, root, child, next_table_index);
	}
	TableIndex function_index(0);
	ExpressionClass function_class;
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		function_index = op->Cast<LogicalAggregate>().aggregate_index;
		function_class = ExpressionClass::BOUND_AGGREGATE;
	} else if (op->type == LogicalOperatorType::LOGICAL_WINDOW) {
		function_index = op->Cast<LogicalWindow>().window_index;
		function_class = ExpressionClass::BOUND_WINDOW;
	} else {
		return;
	}
	column_binding_map_t<LogicalType> serialized_types;
	for (idx_t i = 0; i < op->expressions.size(); i++) {
		auto &expression = op->expressions[i];
		if (!BoundCastExpression::IsCast(*expression)) {
			continue;
		}
		auto &child = BoundCastExpression::ChildMutable(expression->Cast<BoundFunctionExpression>());
		if (child->GetExpressionClass() != function_class) {
			continue;
		}
		serialized_types[ColumnBinding(function_index, ProjectionIndex(i))] = expression->GetReturnType();
		expression = std::move(child);
	}
	if (serialized_types.empty()) {
		return;
	}
	auto old_bindings = op->GetColumnBindings();
	auto projection = LogicalProjection::CreateIdentity(next_table_index, std::move(op));
	next_table_index = TableIndex(next_table_index.index + 1);
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		auto entry = serialized_types.find(old_bindings[i]);
		if (entry != serialized_types.end()) {
			projection->expressions[i] =
			    BoundCastExpression::AddCastToType(context, std::move(projection->expressions[i]), entry->second);
		}
	}
	auto new_bindings = projection->GetColumnBindings();
	// the projection returns the serialized types, so the operators above keep theirs
	projection->ResolveOperatorTypes();
	op = std::move(projection);
	// the operators above read the projection, which itself still reads the aggregate or the window
	ColumnBindingReplacer replacer;
	replacer.AddReplacements(old_bindings, new_bindings);
	replacer.stop_operator = op.get();
	replacer.VisitOperator(*root);
}

} // namespace

BoundStatement Binder::Bind(LogicalPlanStatement &stmt) {
	auto next_table_index = TableIndex(GetMaxTableIndex(*stmt.plan).index + 1);
	RestoreSerializedFunctionTypes(context, stmt.plan, stmt.plan, next_table_index);
	BoundStatement result;
	result.types = stmt.plan->types;
	for (idx_t i = 0; i < result.types.size(); i++) {
		result.names.emplace_back(StringUtil::Format("col%d", i));
	}
	result.plan = std::move(stmt.plan);

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::QUERY_RESULT; // TODO could also be something else

	if (parent) {
		throw InternalException("LogicalPlanStatement should be bound in root binder");
	}
	global_binder_state->bound_tables = GetMaxTableIndex(*result.plan).index + 1;
	return result;
}

} // namespace duckdb
