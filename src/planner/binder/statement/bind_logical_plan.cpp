#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include <algorithm>

namespace duckdb {

namespace {

//! A plan serialized before an aggregate's return type changed gets that aggregate wrapped in a cast to the serialized
//! type, which BoundAggregateExpression::Deserialize puts there. A LogicalAggregate may only hold aggregates, so the
//! cast moves to the references of the aggregate in the operators above it, and the plan keeps its types
class AggregateCastMover : public LogicalOperatorVisitor {
public:
	explicit AggregateCastMover(ClientContext &context) : context(context) {
	}

	void VisitOperator(LogicalOperator &op) override {
		// the children first, so the casts of an aggregate are known before the operators above it
		VisitOperatorChildren(op);
		if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			auto &aggregate = op.Cast<LogicalAggregate>();
			for (idx_t i = 0; i < aggregate.expressions.size(); i++) {
				auto &expression = aggregate.expressions[i];
				if (!BoundCastExpression::IsCast(*expression)) {
					continue;
				}
				auto &child = BoundCastExpression::ChildMutable(expression->Cast<BoundFunctionExpression>());
				if (child->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
					continue;
				}
				ColumnBinding binding(aggregate.aggregate_index, ProjectionIndex(i));
				moved_casts[binding] = make_pair(expression->GetReturnType(), child->GetReturnType());
				expression = std::move(child);
			}
		}
		VisitOperatorExpressions(op);
	}

	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override {
		auto entry = moved_casts.find(expr.Binding());
		if (entry == moved_casts.end() || expr.GetReturnType() != entry->second.first) {
			return nullptr;
		}
		auto reference = expr.Copy();
		reference->SetReturnType(entry->second.second);
		return BoundCastExpression::AddCastToType(context, std::move(reference), entry->second.first);
	}

	bool MovedCasts() const {
		return !moved_casts.empty();
	}

private:
	ClientContext &context;
	//! For each aggregate whose cast moved, the type the plan was serialized with and the type it returns now
	column_binding_map_t<pair<LogicalType, LogicalType>> moved_casts;
};

} // namespace

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

BoundStatement Binder::Bind(LogicalPlanStatement &stmt) {
	AggregateCastMover cast_mover(context);
	cast_mover.VisitOperator(*stmt.plan);
	if (cast_mover.MovedCasts()) {
		stmt.plan->ResolveOperatorTypes();
	}
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
