#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/query_result_stream.hpp"

#if defined(__linux__)
#include <fstream>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <chrono>
#include <condition_variable>
#include <thread>

using namespace duckdb;

// A client that streams a result waits instead of polling while a worker runs the only task
// (duckdblabs/bot-issue-tracker#525)

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

//! The only task of the query waits until the client opened its stream, produces one chunk, then watches the client
//! wait in its fetch while it keeps the task
struct StreamHandoffState {
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
	void StartAndWaitForRelease() {
		unique_lock<mutex> guard(lock);
		started = true;
		scan_thread = std::this_thread::get_id();
		cv.notify_all();
		cv.wait(guard, [&]() { return released; });
	}
	void Release() {
		lock_guard<mutex> guard(lock);
		released = true;
		cv.notify_all();
	}
	bool WaitUntilStarted() {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(10), [&]() { return started; });
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

struct StreamHandoffInfo : TableFunctionInfo {
	explicit StreamHandoffInfo(shared_ptr<StreamHandoffState> state) : state(std::move(state)) {
	}
	shared_ptr<StreamHandoffState> state;
};

struct StreamHandoffBindData : TableFunctionData {
	explicit StreamHandoffBindData(shared_ptr<StreamHandoffState> state) : state(std::move(state)) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<StreamHandoffBindData>(state);
	}
	bool Equals(const FunctionData &other) const override {
		return state == other.Cast<StreamHandoffBindData>().state;
	}
	shared_ptr<StreamHandoffState> state;
};

struct StreamHandoffGlobalState : GlobalTableFunctionState {
	idx_t call = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

} // namespace

TEST_CASE("Test that a streaming client waits for the next unit while a worker runs the only task", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads = 4"));
	auto state = make_shared_ptr<StreamHandoffState>();
	state->client_tid = static_cast<pid_t>(syscall(SYS_gettid));
	if (!std::ifstream("/proc/self/task/" + to_string(state->client_tid) + "/stat")) {
		// the state of the client thread is read from procfs
		WARN("Skipping the test, as /proc/self/task is not readable");
		return;
	}
	TableFunction stream_handoff(
	    "stream_handoff", {},
	    [](ClientContext &, TableFunctionInput &input, DataChunk &output) {
		    auto &global = input.global_state->Cast<StreamHandoffGlobalState>();
		    auto &state = *input.bind_data->Cast<StreamHandoffBindData>().state;
		    auto call = global.call++;
		    if (call == 0) {
			    // the client opens its stream first, so that the chunk is not held for the retention decision
			    state.StartAndWaitForRelease();
		    } else if (call == 1) {
			    // the worker keeps the only task while the client waits in its fetch
			    state.WatchClient();
		    } else {
			    return;
		    }
		    // a full chunk, as operators hold back small chunks to combine them
		    auto writer = FlatVector::Writer<int64_t>(output.data[0], STANDARD_VECTOR_SIZE);
		    for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
			    writer.WriteValue(static_cast<int64_t>(call + 1));
		    }
		    output.SetCardinality(STANDARD_VECTOR_SIZE);
	    },
	    [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
	       vector<Identifier> &names) -> unique_ptr<FunctionData> {
		    return_types.push_back(LogicalType::BIGINT);
		    names.push_back("i");
		    return make_uniq<StreamHandoffBindData>(input.info->Cast<StreamHandoffInfo>().state);
	    },
	    [](ClientContext &, TableFunctionInitInput &) -> unique_ptr<GlobalTableFunctionState> {
		    return make_uniq<StreamHandoffGlobalState>();
	    });
	stream_handoff.function_info = make_shared_ptr<StreamHandoffInfo>(state);
	ExtensionLoader loader(*db.instance, "test_extension");
	loader.RegisterFunction(stream_handoff);

	for (idx_t run = 0; run < 3; run++) {
		state->Reset();
		auto handle = con.Submit("SELECT * FROM stream_handoff()");
		REQUIRE_NO_FAIL(*handle);
		// a worker runs the task, as this thread waits here
		auto started = state->WaitUntilStarted();
		QueryResultStream<> stream(std::move(handle));
		state->Release();
		REQUIRE(started);
		auto first = stream.Fetch();
		REQUIRE(first);
		REQUIRE(first->size() == STANDARD_VECTOR_SIZE);
		REQUIRE(first->GetValue(0, 0) == Value::BIGINT(1));
		auto second = stream.Fetch();
		REQUIRE(second);
		REQUIRE(second->size() == STANDARD_VECTOR_SIZE);
		REQUIRE(second->GetValue(0, 0) == Value::BIGINT(2));
		REQUIRE(!stream.Fetch());
		REQUIRE(state->scan_thread != std::this_thread::get_id());
		// this thread slept while the worker held the task, instead of polling
		REQUIRE(state->client_slept);
	}
}
#endif
