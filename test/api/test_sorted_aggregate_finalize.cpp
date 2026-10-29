#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/arena_allocator.hpp"

using namespace duckdb;

namespace {

//! An order sensitive inner aggregate, the sum of the values weighted by their position
struct WeightedState {
	int64_t seen;
	int64_t weighted;
};

idx_t WeightedSize(AggregateStateInput &) {
	return sizeof(WeightedState);
}

void WeightedInitialize(AggregateStateInput &, data_ptr_t *states, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		*reinterpret_cast<WeightedState *>(states[i]) = {0, 0};
	}
}

void WeightedUpdate(Vector inputs[], AggregateInputData &, idx_t, Vector &states, idx_t count) {
	auto values = inputs[0].Values<int64_t>();
	auto state_ptrs = states.Values<WeightedState *>();
	for (idx_t i = 0; i < count; i++) {
		if (!values[i].IsValid()) {
			continue;
		}
		auto &state = *state_ptrs[i].GetValue();
		state.weighted += (++state.seen) * values[i].GetValue();
	}
}

void WeightedFinalize(Vector &states, AggregateFinalizeInputData &, Vector &result, idx_t count, idx_t offset) {
	auto state_ptrs = states.Values<WeightedState *>();
	auto writer = FlatVector::Writer<int64_t>(result, count, offset);
	for (idx_t i = 0; i < count; i++) {
		auto &state = *state_ptrs[i].GetValue();
		if (state.seen) {
			writer.WriteValue(state.weighted);
		} else {
			writer.WriteNull();
		}
	}
}

struct FinalizeCase {
	idx_t count;
	idx_t offset;
	//! Every state from the 65535th on is empty
	bool empty_tail;
	//! Some states around the batch boundary hold 4099 rows, and some are empty
	bool large;
	//! The states vector is a constant vector
	bool constant;
	const char *name;
};

//! The number of rows the i-th state of the case holds
idx_t RowsOfState(const FinalizeCase &c, idx_t i) {
	if (c.empty_tail && i >= 65535) {
		return 0;
	}
	if (c.large && (i == 65533 || i == 65536 || i == 65539)) {
		return 0;
	}
	if (c.large && (i == 65534 || i == 65535 || i == 65537)) {
		return 4099;
	}
	return 1;
}

} // namespace

TEST_CASE("Sorted aggregate finalize batches more than 65535 states", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET ordered_aggregate_threshold=4096"));
	con.BeginTransaction();

	AggregateFunction inner("weighted", {LogicalType::BIGINT}, LogicalType::BIGINT, WeightedSize, WeightedInitialize,
	                        WeightedUpdate, nullptr, WeightedFinalize, FunctionNullHandling::DEFAULT_NULL_HANDLING);
	BoundAggregateFunction bound_inner(inner);
	auto wrapper = FunctionBinder::BindSortedAggregateState(
	    *con.context, bound_inner, nullptr, LogicalType::STRUCT({{"v", LogicalType::BIGINT}}),
	    {{0, OrderType::ASCENDING, OrderByNullType::NULLS_FIRST}}, 1);
	BoundAggregateFunction sorted(wrapper.first);
	auto bind_data = wrapper.second.get();

	ArenaAllocator arena(Allocator::DefaultAllocator());
	AggregateInputData input(sorted, bind_data, arena);
	AggregateFinalizeInputData finalize_input(sorted, bind_data, arena);
	AggregateStateInput state_input(sorted, bind_data);
	auto &callbacks = sorted.GetCallbacks();
	const auto state_size = callbacks.GetStateSizeCallback()(state_input);

	const vector<FinalizeCase> cases {
	    {0, 7, false, false, false, "no states"},
	    {1, 0, false, false, true, "a constant state"},
	    {1, 7, false, false, false, "offset larger than the count"},
	    {23, 7, false, false, false, "offset smaller than the count"},
	    {65535, 0, false, false, false, "exactly one batch"},
	    {65536, 0, false, false, false, "a second batch of one state"},
	    {65536, 7, false, false, false, "a second batch of one state, with an offset"},
	    {131071, 7, false, false, false, "a third batch of one state, with an offset"},
	    {65540, 7, true, false, false, "an empty second batch"},
	    {65540, 7, false, true, false, "large and empty states around the boundary"},
	};
	for (auto &c : cases) {
		INFO(c.name);

		// allocate and initialize the states
		vector<data_t> storage(c.count * state_size);
		vector<data_ptr_t> addresses;
		for (idx_t i = 0; i < c.count; i++) {
			addresses.push_back(storage.data() + i * state_size);
		}
		callbacks.GetStateInitCallback()(state_input, addresses.data(), c.count);
		Vector states(LogicalType::POINTER, c.count);
		{
			auto writer = FlatVector::Writer<data_ptr_t>(states, c.count);
			for (auto address : addresses) {
				writer.WriteValue(address);
			}
		}

		// feed the i-th state its rows in reverse order, so the sort has to restore it
		vector<Value> expected;
		Vector values(LogicalType::BIGINT, STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < c.count; i++) {
			const auto rows = RowsOfState(c, i);
			int64_t answer = 0;
			for (idx_t k = 0; k < rows; k++) {
				answer += int64_t(k + 1) * int64_t(i * 10 + k + 1);
			}
			expected.push_back(rows ? Value::BIGINT(answer) : Value(LogicalType::BIGINT));
			for (idx_t start = 0; start < rows; start += STANDARD_VECTOR_SIZE) {
				const auto n = MinValue<idx_t>(STANDARD_VECTOR_SIZE, rows - start);
				{
					auto writer = FlatVector::Writer<int64_t>(values, n);
					for (idx_t k = start; k < start + n; k++) {
						writer.WriteValue(int64_t(i * 10 + rows - k));
					}
				}
				Vector state(Value::POINTER(CastPointerToValue(addresses[i])), count_t(n));
				callbacks.GetStateUpdateCallback()(&values, input, 1, state, n);
			}
		}
		if (c.constant) {
			states.Reference(Value::POINTER(CastPointerToValue(addresses[0])), count_t(1));
		}

		// finalize into a result prefilled with a marker, which the offset must preserve
		const auto result_count = c.count + c.offset + 1;
		Vector result(LogicalType::BIGINT, result_count);
		{
			auto writer = FlatVector::Writer<int64_t>(result, result_count);
			for (idx_t i = 0; i < result_count; i++) {
				writer.WriteValue(-777);
			}
		}
		callbacks.GetStateFinalizeCallback()(states, finalize_input, result, c.count, c.offset);

		idx_t wrong = 0;
		for (idx_t i = 0; i < c.offset; i++) {
			wrong += result.GetValue(i) != Value::BIGINT(-777);
		}
		for (idx_t i = 0; i < c.count; i++) {
			auto actual = result.GetValue(i + c.offset);
			wrong += actual.IsNull() != expected[i].IsNull() || (!actual.IsNull() && actual != expected[i]);
		}
		REQUIRE(wrong == 0);
	}
	con.Rollback();
}
