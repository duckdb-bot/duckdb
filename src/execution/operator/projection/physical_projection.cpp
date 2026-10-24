#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/function/scalar/sequence_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

//! Counts the calls of functions that require ordered execution, as nextval, and of currval, also in nested calls and
//! in lambda bodies
static void CountOrderedCalls(const Expression &expr, idx_t &sequence_calls, bool &requires_ordered_execution) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &function = expr.Cast<BoundFunctionExpression>();
		if (function.Function().RequiresOrderedExecution()) {
			requires_ordered_execution = true;
			sequence_calls++;
		} else if (function.Function().GetName() == CurrvalFun::Name) {
			sequence_calls++;
		}
		if (function.Function().HasBindLambdaCallback() && function.BindInfo()) {
			auto lambda = function.BindInfo()->Cast<LambdaFunctionData>().GetLambdaExpression();
			if (lambda) {
				CountOrderedCalls(*lambda, sequence_calls, requires_ordered_execution);
			}
		}
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { CountOrderedCalls(child, sequence_calls, requires_ordered_execution); });
}

//! Whether the select list calls sequence functions that see what the others do, which must then happen in the order
//! of the rows, as when a currval follows the nextval of an earlier row
static bool CallsDependentSequenceFunctions(const vector<unique_ptr<Expression>> &expressions) {
	idx_t sequence_calls = 0;
	bool requires_ordered_execution = false;
	for (auto &expression : expressions) {
		CountOrderedCalls(*expression, sequence_calls, requires_ordered_execution);
	}
	return requires_ordered_execution && sequence_calls > 1;
}

class ProjectionState : public OperatorState {
public:
	explicit ProjectionState(ExecutionContext &context, const vector<unique_ptr<Expression>> &expressions)
	    : executor(context.client, expressions), row_by_row(CallsDependentSequenceFunctions(expressions)),
	      allocator(Allocator::Get(context.client)) {
	}

	ExpressionExecutor executor;
	//! Whether the rows are evaluated one after another
	bool row_by_row;

	void ExecuteRowByRow(DataChunk &input, DataChunk &chunk) {
		if (rows.ColumnCount() == 0) {
			row.InitializeEmpty(input.GetTypes());
			row_result.Initialize(allocator, chunk.GetTypes(), 1);
			rows.Initialize(allocator, chunk.GetTypes());
		}
		rows.Reset();
		SelectionVector sel(1);
		for (idx_t i = 0; i < input.size(); i++) {
			sel.set_index(0, i);
			row.Slice(input, sel, 1);
			row_result.Reset();
			executor.Execute(row, row_result);
			rows.Append(row_result);
		}
		chunk.Reference(rows);
	}

private:
	Allocator &allocator;
	DataChunk row;
	DataChunk row_result;
	DataChunk rows;

public:
	void Finalize(const PhysicalOperator &op, ExecutionContext &context) override {
		context.thread.profiler.Flush(op);
	}

	bool SupportsReuse() const override {
		return true;
	}
};

PhysicalProjection::PhysicalProjection(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                       vector<unique_ptr<Expression>> select_list, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::PROJECTION, std::move(types), estimated_cardinality),
      select_list(std::move(select_list)) {
}

void PhysicalProjection::InitializeOutputChunk(Allocator &allocator, DataChunk &chunk) const {
	// column references only reference their input vector. Only computed columns need a buffer
	vector<bool> initialize;
	bool any_computed = false;
	for (auto &expression : select_list) {
		const bool computed = expression->GetExpressionClass() != ExpressionClass::BOUND_REF;
		initialize.push_back(computed);
		any_computed = any_computed || computed;
	}
	if (!any_computed) {
		chunk.InitializeEmpty(types);
		return;
	}
	chunk.Initialize(allocator, types, initialize);
}

OperatorResultType PhysicalProjection::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                               GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<ProjectionState>();
	if (state.row_by_row) {
		state.ExecuteRowByRow(input, chunk);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	state.executor.Execute(input, chunk);
	return OperatorResultType::NEED_MORE_INPUT;
}

unique_ptr<OperatorState> PhysicalProjection::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<ProjectionState>(context, select_list);
}

bool PhysicalProjection::ParallelOperator() const {
	idx_t sequence_calls = 0;
	bool requires_ordered_execution = false;
	for (auto &expression : select_list) {
		CountOrderedCalls(*expression, sequence_calls, requires_ordered_execution);
	}
	return !requires_ordered_execution;
}

InsertionOrderPreservingMap<string> PhysicalProjection::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	string projections;
	for (idx_t i = 0; i < select_list.size(); i++) {
		if (i > 0) {
			projections += "\n";
		}
		auto &expr = select_list[i];
		projections += expr->GetName();
	}
	result["__projections__"] = projections;
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
