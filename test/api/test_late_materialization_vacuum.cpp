#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <chrono>
#include <condition_variable>
#include <thread>

using namespace duckdb;

// The scan that late materialization adds fetches its rows in batches, and a checkpoint between two batches must not
// move rows under it (duckdblabs/bot-issue-tracker#549)

#ifndef DUCKDB_NO_THREADS
namespace {

//! Lets the query stop after its first result batch, until a checkpoint ran
struct PauseState {
	mutex lock;
	std::condition_variable cv;
	bool enabled = false;
	bool paused = false;
	bool released = false;
};

PauseState &GetPauseState() {
	static PauseState state;
	return state;
}

void PauseFunction(DataChunk &args, ExpressionState &, Vector &result) {
	auto writer = FlatVector::Writer<int64_t>(result, args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		writer.WriteValue(1);
	}
	auto &state = GetPauseState();
	unique_lock<mutex> guard(state.lock);
	if (!state.enabled || state.paused) {
		return;
	}
	state.paused = true;
	state.cv.notify_all();
	state.cv.wait_for(guard, std::chrono::seconds(10), [&]() { return state.released; });
}

} // namespace

TEST_CASE("Test a checkpoint between the fetch batches of late materialization", "[api]") {
	auto path = TestCreatePath("late_materialization_vacuum.db");
	DeleteDatabase(path);
	DuckDB db(nullptr);
	Connection con(db);
	Connection checkpointer(db);

	ScalarFunction pause("pause_once", {LogicalType::BIGINT}, LogicalType::BIGINT, PauseFunction);
	pause.SetFallible();
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(pause);

	// a storage version whose vacuum moves rows, and tables without indexes
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + path +
	                          "' AS d (STORAGE_VERSION 'v1.5.0', ROW_GROUP_SIZE 2048, VACUUM_REBUILD_INDEXES 0)"));
	REQUIRE_NO_FAIL(con.Query("USE d"));
	REQUIRE_NO_FAIL(con.Query("SET max_vacuum_tasks = 0"));
	REQUIRE_NO_FAIL(con.Query("SET late_materialization_max_rows = 10000"));
	REQUIRE_NO_FAIL(con.Query("SET dynamic_or_filter_threshold = 10000"));
	REQUIRE_NO_FAIL(con.Query("SET index_scan_max_count = 10000"));
	REQUIRE_NO_FAIL(con.Query("SET index_scan_percentage = 0"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT i, (i * 7919) % 16384 AS ord, 'payload' || i AS s "
	                          "FROM range(16384) r(i)"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT d"));
	REQUIRE_NO_FAIL(con.Query("DELETE FROM t WHERE i < 2048"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA force_checkpoint"));
	REQUIRE_NO_FAIL(con.Query("SET GLOBAL max_vacuum_tasks = 99"));

	const string query = "SELECT i, s, pause_once(i) AS p FROM t ORDER BY ord LIMIT 4097";
	// the expected rows, read before the checkpoint, whose transaction has to end for vacuum to move rows
	auto expected = con.Query("SELECT i, s, 1::BIGINT AS p FROM t ORDER BY ord LIMIT 4097")->ToString();

	// the checkpoint vacuums only when nothing committed after the query started, so nothing else runs meanwhile
	auto &state = GetPauseState();
	state.enabled = true;
	unique_ptr<QueryResult> result;
	std::thread reader([&]() { result = con.Query(query); });
	bool paused;
	{
		unique_lock<mutex> guard(state.lock);
		paused = state.cv.wait_for(guard, std::chrono::seconds(10), [&]() { return state.paused; });
	}
	// vacuum would move the rows that the remaining batches still have to fetch
	auto checkpoint = checkpointer.Query("CHECKPOINT d");
	{
		lock_guard<mutex> guard(state.lock);
		state.released = true;
		state.cv.notify_all();
	}
	reader.join();
	state.enabled = false;
	REQUIRE(paused);
	REQUIRE_NO_FAIL(*checkpoint);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 4097);
	REQUIRE(result->ToString() == expected);
}
#endif
