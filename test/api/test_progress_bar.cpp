#ifndef DUCKDB_NO_THREADS

#include "catch.hpp"
#include "duckdb/common/progress_bar/display/terminal_progress_bar_display.hpp"
#include "duckdb/common/progress_bar/progress_bar.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "test_helpers.hpp"

#include <duckdb/execution/executor.hpp>
#include <future>
#include <thread>

using namespace duckdb;

TEST_CASE("Test terminal progress ETA estimate", "[progress-bar]") {
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(0, 10) == Approx(2147483647.0));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(25, 10) == Approx(30));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(50, 100) == Approx(100));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(100, 10) == Approx(0));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(50, 100, 1) == Approx(50));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(50, 100, 4) == Approx(50));
	REQUIRE(TerminalProgressBarDisplay::EstimateRemainingSeconds(50, 100, 0.1) == Approx(200));
}

class TestProgressBar {
	class TestFailure {
		using failure_callback = std::function<void()>;

	public:
		TestFailure() : callback(nullptr) {
		}

	public:
		bool IsSet() {
			return callback != nullptr;
		}
		void SetError(failure_callback failure) {
			if (!callback) {
				callback = failure;
			}
		}
		void ThrowError() {
			D_ASSERT(IsSet());
			callback();
		}

	private:
		failure_callback callback;
	};

public:
	explicit TestProgressBar(ClientContext *context) : context(context) {
	}

	ClientContext *context;
	atomic<bool> stop;
	std::thread check_thread;
	TestFailure error;

	void CheckProgressThread() {
		double prev_percentage = -1;
		uint64_t total_cardinality = 0;
		uint64_t cur_rows_read = 0;
		while (!stop) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			auto query_progress = context->GetQueryProgress();
			double new_percentage = query_progress.GetPercentage();
			if (new_percentage < prev_percentage && new_percentage != -1) {
				error.SetError([new_percentage, prev_percentage]() { REQUIRE(new_percentage >= prev_percentage); });
			}
			if (new_percentage > 100) {
				error.SetError([new_percentage]() { REQUIRE(new_percentage <= 100); });
			}
			cur_rows_read = query_progress.GetRowsProcessed();
			total_cardinality = query_progress.GetTotalRowsToProcess();
			if (cur_rows_read > total_cardinality) {
				error.SetError([cur_rows_read, total_cardinality]() { REQUIRE(cur_rows_read <= total_cardinality); });
			}
		}
		if (cur_rows_read != total_cardinality) {
			if (std::getenv("FORCE_ASYNC_SINK_SOURCE") != nullptr) {
				return;
			}
			error.SetError([cur_rows_read, total_cardinality]() { REQUIRE(cur_rows_read == total_cardinality); });
		}
	}
	void Start() {
		stop = false;
		check_thread = std::thread(&TestProgressBar::CheckProgressThread, this);
	}
	void End() {
		stop = true;
		check_thread.join();
		if (error.IsSet()) {
			error.ThrowError();
			// This should never be reached, ThrowError() should contain a failing REQUIRE statement
			REQUIRE(false);
		}
	}
};

TEST_CASE("Test Progress Bar Fast", "[progress-bar]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NOTHROW(con.context->GetQueryProgress());

	TestProgressBar test_progress(con.context.get());

	REQUIRE_NOTHROW(con.context->GetQueryProgress());

	REQUIRE_NO_FAIL(con.Query("create  table tbl as select range a, mod(range,10) b from range(10000);"));
	REQUIRE_NO_FAIL(con.Query("create  table tbl_2 as select range a from range(10000);"));

	REQUIRE_NO_FAIL(con.Query("PRAGMA progress_bar_time=10"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));
	// Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	// Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	// Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	auto stream = OpenStream(con, "select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	// The query only completes once the stream is consumed
	while (stream->Fetch()) {
	}
	test_progress.End();
	REQUIRE(!stream->HasError());

	// Test Multiple threads
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=2"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA verify_parallelism"));

	// Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	// Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	// Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	stream = OpenStream(con, "select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	while (stream->Fetch()) {
	}
	test_progress.End();
	REQUIRE(!stream->HasError());
}

TEST_CASE("Test Progress Bar", "[progress-bar][.]") {
	DuckDB db(nullptr);
	Connection con(db);
	TestProgressBar test_progress(con.context.get());
	REQUIRE_NO_FAIL(con.Query("create  table tbl as select range a, mod(range,10) b from range(10000000);"));
	REQUIRE_NO_FAIL(con.Query("create  table tbl_2 as select range a from range(10000000);"));

	REQUIRE_NO_FAIL(con.Query("PRAGMA progress_bar_time=10"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));
	// Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	// Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	// Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	auto stream = OpenStream(con, "select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	// The query only completes once the stream is consumed
	while (stream->Fetch()) {
	}
	test_progress.End();
	REQUIRE(!stream->HasError());

	// Test Multiple threads
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA verify_parallelism"));

	// Simple Aggregation
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl"));
	test_progress.End();

	// Simple Join
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)"));
	test_progress.End();

	// Subquery
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(a) from tbl_2)"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("select count(*) from tbl where a = (select min(b) from tbl)"));
	test_progress.End();

	// Stream result
	test_progress.Start();
	stream = OpenStream(con, "select count(*) from tbl inner join tbl_2 on (tbl.a = tbl_2.a)");
	while (stream->Fetch()) {
	}
	test_progress.End();
	REQUIRE(!stream->HasError());
}

TEST_CASE("Test Progress Bar CSV", "[progress-bar][.]") {
	DuckDB db(nullptr);
	Connection con(db);

	TestProgressBar test_progress(con.context.get());
	REQUIRE_NO_FAIL(con.Query("PRAGMA progress_bar_time=1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));

	// Create Tables From CSVs
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE test AS SELECT * FROM read_csv_auto ('data/csv/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE test_2 AS SELECT * FROM  read_csv('data/csv/test/test.csv', columns=STRUCT_PACK(a "
	              ":= 'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	// Insert into existing tables
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM read_csv_auto('data/csv/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(
	    con.Query("INSERT INTO test SELECT * FROM  read_csv('data/csv/test/test.csv', columns=STRUCT_PACK(a := "
	              "'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	// copy from
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("COPY test FROM 'data/csv/test/test.csv'"));
	test_progress.End();

	// Repeat but in parallel
	REQUIRE_NO_FAIL(con.Query("DROP TABLE test"));
	REQUIRE_NO_FAIL(con.Query("DROP TABLE test_2"));

	// Test Multiple threads
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA verify_parallelism"));
	// Create Tables From CSVs
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE test AS SELECT * FROM read_csv_auto ('data/csv/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE test_2 AS SELECT * FROM  read_csv('data/csv/test/test.csv', columns=STRUCT_PACK(a "
	              ":= 'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	// Insert into existing tables
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM read_csv_auto('data/csv/test/test.csv')"));
	test_progress.End();

	test_progress.Start();
	REQUIRE_NO_FAIL(
	    con.Query("INSERT INTO test SELECT * FROM  read_csv('data/csv/test/test.csv', columns=STRUCT_PACK(a := "
	              "'INTEGER', b := 'INTEGER', c := 'VARCHAR'), sep=',', auto_detect='false')"));
	test_progress.End();

	// copy from
	test_progress.Start();
	REQUIRE_NO_FAIL(con.Query("COPY test FROM 'data/csv/test/test.csv'"));
	test_progress.End();
}

TEST_CASE("Test progress is not capped by a low cardinality estimate", "[progress-bar]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET enable_progress_bar=true"));
	REQUIRE_NO_FAIL(con.Query("SET enable_progress_bar_print=false"));
	// j is unique and not correlated with the row order, so a filter on j is estimated to return very few rows while
	// zonemaps cannot prune it: the query scans the whole table
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE tbl AS SELECT i, (i * 7919) % 1000000 AS j FROM range(1000000) t(i)"));

	auto check_progress = [&](const string &query) {
		INFO(query);
		auto result = con.Submit(query);
		result->Materialize();
		// the call that finishes the query also resets its progress, so keep the highest value seen before that
		double max_percentage = -1;
		while (!IsTerminal(result->ExecuteTask())) {
			max_percentage = MaxValue<double>(max_percentage, con.context->GetQueryProgress().GetPercentage());
		}
		REQUIRE(!result->HasError());
		CAPTURE(max_percentage);
		CHECK(max_percentage >= 99);
	};
	check_progress("SELECT i FROM tbl WHERE j = 12345");
	check_progress("SELECT count(*) FROM tbl WHERE j = 12345");
	check_progress("SELECT i FROM tbl WHERE j = 12345 ORDER BY i");
	check_progress("SELECT a.i FROM tbl a JOIN tbl b USING (i) WHERE a.j = 12345");
	check_progress("CREATE TEMP TABLE ctas AS SELECT i FROM tbl WHERE j = 12345");
}

//! Counts the calls a progress bar display gets
struct CountingProgressBarDisplay : public ProgressBarDisplay {
	CountingProgressBarDisplay(idx_t &updates, idx_t &finishes, idx_t &updates_after_finish)
	    : updates(updates), finishes(finishes), updates_after_finish(updates_after_finish) {
	}
	void Update(double percentage) override {
		updates++;
		if (finishes > 0) {
			updates_after_finish++;
		}
	}
	void Finish() override {
		finishes++;
	}
	idx_t &updates;
	idx_t &finishes;
	idx_t &updates_after_finish;
};

TEST_CASE("Test progress stays below 100 until the result is ready", "[progress-bar]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET enable_progress_bar=true"));
	REQUIRE_NO_FAIL(con.Query("SET enable_progress_bar_print=false"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE tbl AS SELECT i, (i * 7919) % 1000000 AS j FROM range(1000000) t(i)"));

	// the source of the only counted pipeline reports every row done before the result collector finishes, so the
	// progress of a running query must stay below 100
	auto check_progress = [&](const string &query) {
		INFO(query);
		auto result = con.Submit(query);
		result->Materialize();
		bool saw_progress = false;
		while (!IsTerminal(result->ExecuteTask())) {
			auto percentage = con.context->GetQueryProgress().GetPercentage();
			if (percentage >= 0) {
				saw_progress = true;
				CHECK(percentage < 100);
			}
		}
		REQUIRE(!result->HasError());
		CHECK(saw_progress);
	};
	check_progress("SELECT i FROM tbl WHERE j = 12345");
	check_progress("SELECT count(*) FROM tbl WHERE j = 12345");
	check_progress("CREATE TEMP TABLE ctas AS SELECT i FROM tbl WHERE j = 12345");

	// once the query is complete its progress is 100, also before the result is read
	auto check_completion = [&](const string &query) {
		INFO(query);
		QueryResultStream<> stream(con.Submit(query));
		QueryResultState state;
		idx_t steps = 0;
		do {
			state = stream.ExecuteTask();
			if (state == QueryResultState::BLOCKED || state == QueryResultState::NO_TASKS_AVAILABLE) {
				stream.WaitForTask();
			}
			REQUIRE(++steps < 100000);
		} while (!IsTerminal(state));
		REQUIRE(!stream.HasError());
		CHECK(con.context->GetQueryProgress().GetPercentage() == 100);
	};
	check_completion("SELECT i FROM tbl WHERE j = 12345");
	check_completion("SELECT count(*) FROM tbl WHERE j = 12345");

	// a result that is ready while its query still runs, here waiting for its retention decision, keeps the real
	// progress
	auto check_partial_ready = [&](const string &query) {
		INFO(query);
		auto result = con.Submit(query);
		QueryResultState state;
		idx_t steps = 0;
		do {
			state = result->ExecuteTask();
			if (state == QueryResultState::BLOCKED || state == QueryResultState::NO_TASKS_AVAILABLE) {
				result->WaitForTask();
			}
			REQUIRE(++steps < 100000);
		} while (state != QueryResultState::READY && !IsTerminal(state));
		REQUIRE(state == QueryResultState::READY);
		auto percentage = con.context->GetQueryProgress().GetPercentage();
		CHECK(percentage > 0);
		CHECK(percentage < 100);
	};
	check_partial_ready("SELECT i FROM tbl");

	// a stream whose execution a worker completes while its result waits in the buffer reports 100 when polled
	auto check_buffered_completion = [&](const string &query) {
		INFO(query);
		REQUIRE_NO_FAIL(con.Query("SET streaming_buffer_size='1B'"));
		QueryResultStream<> stream(con.Submit(query));
		QueryResultState state;
		idx_t steps = 0;
		do {
			state = stream.ExecuteTask();
			if (state == QueryResultState::BLOCKED || state == QueryResultState::NO_TASKS_AVAILABLE) {
				stream.WaitForTask();
			}
			REQUIRE(++steps < 100000);
		} while (state != QueryResultState::READY && !IsTerminal(state));
		REQUIRE(state == QueryResultState::READY);
		// the remaining tasks, as a worker thread would run them
		TaskScheduler::GetScheduler(*con.context).ExecuteTasks(100000);
		REQUIRE(con.context->GetExecutor().ExecutionIsFinished());
		CHECK(stream.Poll() == QueryResultState::READY);
		CHECK(con.context->GetQueryProgress().GetPercentage() == 100);
		CHECK(stream.ExecuteTask() == QueryResultState::READY);
		CHECK(con.context->GetQueryProgress().GetPercentage() == 100);
		while (stream.Fetch()) {
		}
		REQUIRE(!stream.HasError());
		REQUIRE_NO_FAIL(con.Query("RESET streaming_buffer_size"));
	};
	check_buffered_completion("SELECT count(*) FROM tbl WHERE j = 12345");

	// with printing enabled, a result that is buffered while its execution runs leaves the display running, and the
	// display finishes once, at the end
	REQUIRE_NO_FAIL(con.Query("SET enable_progress_bar_print=true"));
	REQUIRE_NO_FAIL(con.Query("SET progress_bar_time=0"));
	REQUIRE_NO_FAIL(con.Query("SET streaming_buffer_size='1B'"));
	idx_t display_updates = 0;
	idx_t display_finishes = 0;
	idx_t display_updates_after_finish = 0;
	auto &config = ClientConfig::GetConfig(*con.context);
	config.display_create_func = [&]() -> unique_ptr<ProgressBarDisplay> {
		return make_uniq<CountingProgressBarDisplay>(display_updates, display_finishes, display_updates_after_finish);
	};
	{
		QueryResultStream<> stream(con.Submit("SELECT i FROM tbl"));
		QueryResultState state;
		idx_t steps = 0;
		do {
			state = stream.ExecuteTask();
			if (state == QueryResultState::BLOCKED || state == QueryResultState::NO_TASKS_AVAILABLE) {
				stream.WaitForTask();
			}
			REQUIRE(++steps < 100000);
		} while (state != QueryResultState::READY && !IsTerminal(state));
		REQUIRE(state == QueryResultState::READY);
		REQUIRE(!con.context->GetExecutor().ExecutionIsFinished());
		CHECK(display_finishes == 0);
		while (stream.Fetch()) {
		}
		REQUIRE(!stream.HasError());
	}
	CHECK(display_updates > 0);
	CHECK(display_finishes == 1);
	CHECK(display_updates_after_finish == 0);
	REQUIRE_NO_FAIL(con.Query("RESET enable_progress_bar_print"));
	REQUIRE_NO_FAIL(con.Query("RESET progress_bar_time"));
	REQUIRE_NO_FAIL(con.Query("RESET streaming_buffer_size"));
	config.display_create_func = nullptr;
}

#endif
