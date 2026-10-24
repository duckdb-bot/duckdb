#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/query_profiler.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <time.h>
#endif

#include <chrono>
#include <condition_variable>
#include <thread>

using namespace duckdb;

// A client thread whose query has no task left for it waits instead of polling (duckdb/duckdb#22178)

#if (defined(__linux__) || defined(__APPLE__)) && !defined(DUCKDB_NO_THREADS)
namespace {

double ThreadSeconds() {
	timespec now;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
	return double(now.tv_sec) + double(now.tv_nsec) / 1e9;
}

double WallSeconds() {
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return double(now.tv_sec) + double(now.tv_nsec) / 1e9;
}

//! The only task of the query tells the client which thread runs it, and waits until the client lets it go on
struct HandoffState {
	mutex lock;
	std::condition_variable cv;
	bool started = false;
	bool released = false;
	std::thread::id scan_thread;

	void Reset() {
		lock_guard<mutex> guard(lock);
		started = false;
		released = false;
	}
	bool WaitUntilStarted() {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(10), [&]() { return started; });
	}
	void Release() {
		lock_guard<mutex> guard(lock);
		released = true;
		cv.notify_all();
	}
	void StartAndWaitForRelease() {
		unique_lock<mutex> guard(lock);
		started = true;
		scan_thread = std::this_thread::get_id();
		cv.notify_all();
		cv.wait(guard, [&]() { return released; });
	}
};

struct HandoffInfo : TableFunctionInfo {
	explicit HandoffInfo(shared_ptr<HandoffState> state) : state(std::move(state)) {
	}
	shared_ptr<HandoffState> state;
};

struct HandoffBindData : TableFunctionData {
	explicit HandoffBindData(shared_ptr<HandoffState> state) : state(std::move(state)) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<HandoffBindData>(state);
	}
	bool Equals(const FunctionData &other) const override {
		return state == other.Cast<HandoffBindData>().state;
	}
	shared_ptr<HandoffState> state;
};

struct HandoffGlobalState : GlobalTableFunctionState {
	bool done = false;
	idx_t MaxThreads() const override {
		return 1;
	}
};

} // namespace

TEST_CASE("Test that the client thread does not poll while a worker runs the only task", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads = 4"));
	auto state = make_shared_ptr<HandoffState>();
	TableFunction handoff(
	    "handoff", {},
	    [](ClientContext &, TableFunctionInput &input, DataChunk &output) {
		    auto &global = input.global_state->Cast<HandoffGlobalState>();
		    if (global.done) {
			    return;
		    }
		    global.done = true;
		    input.bind_data->Cast<HandoffBindData>().state->StartAndWaitForRelease();
		    // the worker keeps the only task for a while
		    std::this_thread::sleep_for(std::chrono::milliseconds(200));
		    auto writer = FlatVector::Writer<int64_t>(output.data[0], 1);
		    writer.WriteValue(42);
		    output.SetChildCardinality(1);
	    },
	    [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
	       vector<Identifier> &names) -> unique_ptr<FunctionData> {
		    return_types.push_back(LogicalType::BIGINT);
		    names.push_back("i");
		    return make_uniq<HandoffBindData>(input.info->Cast<HandoffInfo>().state);
	    },
	    [](ClientContext &, TableFunctionInitInput &) -> unique_ptr<GlobalTableFunctionState> {
		    return make_uniq<HandoffGlobalState>();
	    });
	handoff.function_info = make_shared_ptr<HandoffInfo>(state);
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(handoff);
	// the profiler records how long threads waited for a task
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));

	for (idx_t run = 0; run < 3; run++) {
		state->Reset();
		auto result = con.Submit("SELECT * FROM handoff()");
		// a worker runs the task, as this thread waits here
		auto started = state->WaitUntilStarted();
		auto wall_start = WallSeconds();
		auto thread_start = ThreadSeconds();
		state->Release();
		result->Complete();
		auto thread = ThreadSeconds() - thread_start;
		auto wall = WallSeconds() - wall_start;
		REQUIRE(started);
		REQUIRE(state->scan_thread != std::this_thread::get_id());
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
		// this thread waits while the worker holds the task, instead of polling
		REQUIRE(QueryProfiler::Get(*con.context).GetQueryMetrics().blocked_thread_time > 0.5 * wall);
		REQUIRE(thread < 0.2 * wall);
	}
}
#endif
