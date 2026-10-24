#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#if defined(__linux__)
#include <fstream>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <chrono>
#include <condition_variable>
#include <thread>

using namespace duckdb;

// A client thread whose query has no task left for it waits instead of polling (duckdb/duckdb#22178)

#if defined(__linux__) && !defined(DUCKDB_NO_THREADS)
namespace {

//! Whether the kernel has the thread sleeping, as a thread that waits on a condition variable is
bool ThreadIsSleeping(pid_t tid) {
	std::ifstream stat("/proc/self/task/" + to_string(tid) + "/stat");
	string line;
	if (!std::getline(stat, line)) {
		return false;
	}
	// the state follows the command name, which is in parentheses and may contain spaces
	auto end_of_name = line.rfind(')');
	return end_of_name != string::npos && end_of_name + 2 < line.size() && line[end_of_name + 2] == 'S';
}

//! The only task of the query waits until the client lets it go on, and then watches the client while it holds the
//! task
struct HandoffState {
	mutex lock;
	std::condition_variable cv;
	pid_t client_tid = 0;
	bool started = false;
	bool released = false;
	std::thread::id scan_thread;
	bool client_slept = false;

	void Reset() {
		lock_guard<mutex> guard(lock);
		started = false;
		released = false;
		client_slept = false;
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
	//! A polling client is never asleep, a waiting one is asleep for most of the time
	void WatchClient() {
		static constexpr idx_t SAMPLES_ASLEEP = 10;
		idx_t asleep = 0;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			asleep = ThreadIsSleeping(client_tid) ? asleep + 1 : 0;
			if (asleep >= SAMPLES_ASLEEP) {
				client_slept = true;
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
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
	state->client_tid = static_cast<pid_t>(syscall(SYS_gettid));
	if (!std::ifstream("/proc/self/task/" + to_string(state->client_tid) + "/stat")) {
		// the state of the client thread is read from procfs
		WARN("Skipping the test, as /proc/self/task is not readable");
		return;
	}
	TableFunction handoff(
	    "handoff", {},
	    [](ClientContext &, TableFunctionInput &input, DataChunk &output) {
		    auto &global = input.global_state->Cast<HandoffGlobalState>();
		    if (global.done) {
			    return;
		    }
		    global.done = true;
		    auto &state = *input.bind_data->Cast<HandoffBindData>().state;
		    state.StartAndWaitForRelease();
		    // the worker keeps the only task while it watches the client
		    state.WatchClient();
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

	for (idx_t run = 0; run < 3; run++) {
		state->Reset();
		auto result = con.Submit("SELECT * FROM handoff()");
		// a worker runs the task, as this thread waits here
		auto started = state->WaitUntilStarted();
		state->Release();
		result->Complete();
		REQUIRE(started);
		REQUIRE(state->scan_thread != std::this_thread::get_id());
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
		// this thread sleeps while the worker holds the task, instead of polling
		REQUIRE(state->client_slept);
	}
}
#endif
