#include "capi_tester.hpp"
#include "duckdb.h"
#include "result_wait_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test pending statements in C API", "[capi]") {
	CAPITester tester;
	CAPIPrepared prepared;
	CAPIPending pending;
	duckdb::unique_ptr<CAPIResult> result;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE(prepared.Prepare(tester, "SELECT SUM(i) FROM range(1000000) tbl(i)"));
	REQUIRE(pending.Pending(prepared));

	while (true) {
		auto state = pending.ExecuteTask();
		REQUIRE(state != DUCKDB_PENDING_ERROR);
		if (duckdb_pending_execution_is_finished(state)) {
			break;
		}
	}

	result = pending.Execute();
	REQUIRE(result);
	REQUIRE(!result->HasError());
	REQUIRE(result->Fetch<int64_t>(0, 0) == 499999500000LL);
}

TEST_CASE("Test polling a pending statement that the workers already finished", "[capi]") {
	// A statement whose store is retained at submission finishes without the consumer stepping it.
	// Polling it then reports the result as ready, not as an error
	CAPITester tester;
	CAPIPrepared prepared;
	CAPIPending pending;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE(prepared.Prepare(tester, "CREATE TABLE polled AS SELECT i FROM range(10) t(i)"));
	REQUIRE(pending.Pending(prepared));

	// Bounded: a state that never becomes ready fails the test instead of hanging the suite
	duckdb_pending_state state = DUCKDB_PENDING_RESULT_NOT_READY;
	for (idx_t i = 0; i < 1000000 && state != DUCKDB_PENDING_RESULT_READY; i++) {
		state = pending.CheckState();
		REQUIRE(state != DUCKDB_PENDING_ERROR);
	}
	REQUIRE(state == DUCKDB_PENDING_RESULT_READY);

	auto result = pending.Execute();
	REQUIRE(result);
	REQUIRE(!result->HasError());

	result = tester.Query("SELECT count(*) FROM polled");
	REQUIRE(result->Fetch<int64_t>(0, 0) == 10);
}

TEST_CASE("Test destroying a pending insert that polling reported ready leaves no rows", "[capi]") {
	// Polling never completes a statement, even after the workers finished executing it
	CAPITester tester;
	CAPIPrepared prepared;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("SET threads=2"));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t(i BIGINT)"));
	REQUIRE(prepared.Prepare(tester, "INSERT INTO t SELECT i FROM range(1000) t(i)"));
	{
		CAPIPending pending;
		REQUIRE(pending.Pending(prepared));
		Deadline deadline;
		duckdb_pending_state state;
		while ((state = pending.CheckState()) != DUCKDB_PENDING_RESULT_READY) {
			REQUIRE(state != DUCKDB_PENDING_ERROR);
			REQUIRE(!deadline.Passed());
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		REQUIRE(duckdb_pending_execution_is_finished(state));
	}

	auto result = tester.Query("SELECT count(*) FROM t");
	REQUIRE(result->Fetch<int64_t>(0, 0) == 0);
}

TEST_CASE("Test destroying an unfinished pending insert leaves no rows", "[capi]") {
	CAPITester tester;
	CAPIPrepared prepared;

	REQUIRE(tester.OpenDatabase(nullptr));
	// Without worker threads the insert only advances when this thread steps it
	REQUIRE_NO_FAIL(tester.Query("SET threads=1"));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t(i BIGINT)"));
	REQUIRE(prepared.Prepare(tester, "INSERT INTO t SELECT i FROM range(1000000) t(i)"));
	{
		CAPIPending pending;
		REQUIRE(pending.Pending(prepared));
		for (idx_t step = 0; step < 5; step++) {
			auto state = pending.ExecuteTask();
			REQUIRE(state != DUCKDB_PENDING_ERROR);
			REQUIRE(!duckdb_pending_execution_is_finished(state));
		}
	}

	auto result = tester.Query("SELECT count(*) FROM t");
	REQUIRE(result->Fetch<int64_t>(0, 0) == 0);
}

TEST_CASE("Test abandoning an unfinished pending insert inside a transaction invalidates it", "[capi]") {
	CAPITester tester;
	CAPIPrepared prepared;

	REQUIRE(tester.OpenDatabase(nullptr));
	// Without worker threads the insert only advances when this thread steps it
	REQUIRE_NO_FAIL(tester.Query("SET threads=1"));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t(i BIGINT)"));
	REQUIRE(prepared.Prepare(tester, "INSERT INTO t SELECT i FROM range(1000000) t(i)"));
	REQUIRE_NO_FAIL(tester.Query("BEGIN TRANSACTION"));

	auto pending = make_uniq<CAPIPending>();
	REQUIRE(pending->Pending(prepared));
	for (idx_t step = 0; step < 5; step++) {
		auto state = pending->ExecuteTask();
		REQUIRE(state != DUCKDB_PENDING_ERROR);
		REQUIRE(!duckdb_pending_execution_is_finished(state));
	}
	pending.reset();

	auto next = tester.Query("SELECT 42");
	REQUIRE(next->HasError());
	REQUIRE(string(next->ErrorMessage()).find("aborted") != string::npos);
	REQUIRE_NO_FAIL(tester.Query("ROLLBACK"));
	auto result = tester.Query("SELECT count(*) FROM t");
	REQUIRE(result->Fetch<int64_t>(0, 0) == 0);
}

TEST_CASE("Test a statement is refused while a pending insert has not completed", "[capi]") {
	CAPITester tester;
	CAPIPrepared prepared;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("SET threads=1"));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t(i BIGINT)"));
	REQUIRE(prepared.Prepare(tester, "INSERT INTO t SELECT i FROM range(1000000) t(i)"));
	REQUIRE_NO_FAIL(tester.Query("BEGIN TRANSACTION"));

	CAPIPending pending;
	REQUIRE(pending.Pending(prepared));
	for (idx_t step = 0; step < 5; step++) {
		auto state = pending.ExecuteTask();
		REQUIRE(state != DUCKDB_PENDING_ERROR);
		REQUIRE(!duckdb_pending_execution_is_finished(state));
	}
	SECTION("preparing another statement") {
		duckdb_prepared_statement other;
		REQUIRE(duckdb_prepare(tester.connection, "SELECT 42", &other) == DuckDBError);
		REQUIRE(string(duckdb_prepare_error(other)).find("connection has an open result") != string::npos);
		duckdb_destroy_prepare(&other);
	}
	SECTION("running another query") {
		auto refused = tester.Query("SELECT 42");
		REQUIRE(refused->HasError());
		REQUIRE(refused->ErrorType() == DUCKDB_ERROR_RESOURCE_IN_USE);
	}
	SECTION("committing") {
		auto refused = tester.Query("COMMIT");
		REQUIRE(refused->HasError());
		REQUIRE(refused->ErrorType() == DUCKDB_ERROR_RESOURCE_IN_USE);
	}

	// Kept alive across the COMMIT, so that completing the insert, not destroying its result, frees the connection
	auto completed = pending.Execute();
	REQUIRE_NO_FAIL(*completed);
	REQUIRE_NO_FAIL(tester.Query("COMMIT"));
	auto result = tester.Query("SELECT count(*) FROM t");
	REQUIRE(result->Fetch<int64_t>(0, 0) == 1000000);
}

TEST_CASE("Test a zero-row streaming pending query stepped to ready has completed", "[capi]") {
	CAPITester tester;
	CAPIPrepared prepared;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("SET threads=1"));
	REQUIRE_NO_FAIL(tester.Query("CREATE SEQUENCE s"));
	REQUIRE_NO_FAIL(tester.Query("BEGIN TRANSACTION"));
	REQUIRE(prepared.Prepare(tester, "SELECT n FROM (SELECT nextval('s') n FROM range(5)) q WHERE n > 100"));
	{
		CAPIPending pending;
		REQUIRE(pending.PendingStreaming(prepared));
		Deadline deadline;
		duckdb_pending_state state;
		while (!duckdb_pending_execution_is_finished(state = pending.ExecuteTask())) {
			REQUIRE(state != DUCKDB_PENDING_ERROR);
			REQUIRE(!deadline.Passed());
		}
	}

	REQUIRE_NO_FAIL(tester.Query("SELECT 42"));
	REQUIRE_NO_FAIL(tester.Query("COMMIT"));
}

TEST_CASE("Test a pending update planned before an index on its column was created", "[capi]") {
	CAPITester tester;
	CAPIPrepared prepared;
	CAPIPending pending;

	REQUIRE(tester.OpenDatabase(nullptr));
	// Without worker threads the update only runs when it is executed
	REQUIRE_NO_FAIL(tester.Query("SET threads=1"));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t (pad VARCHAR, id INTEGER)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO t SELECT 'x', i FROM range(1, 10001) t(i)"));
	// Planned while the column has no index, so the update writes in place
	REQUIRE(prepared.Prepare(tester, "UPDATE t SET id = 30000000 WHERE id = 5"));
	REQUIRE(pending.Pending(prepared));

	duckdb_connection index_connection;
	REQUIRE(duckdb_connect(tester.database, &index_connection) == DuckDBSuccess);
	CAPIResult index_result;
	index_result.Query(index_connection, "CREATE INDEX idx ON t (id)");
	REQUIRE(!index_result.HasError());
	duckdb_disconnect(&index_connection);

	auto result = pending.Execute();
	REQUIRE(result);
	REQUIRE(result->HasError());
	REQUIRE(string(result->ErrorMessage()).find("Transaction conflict") != string::npos);

	// The index finds the rows that a scan finds
	for (string key : {"5", "30000000"}) {
		auto lookup = tester.Query("SELECT count(*) FROM t WHERE id = " + key);
		auto scan = tester.Query("SELECT count(*) FROM t WHERE id::VARCHAR = '" + key + "'");
		REQUIRE_NO_FAIL(*lookup);
		REQUIRE_NO_FAIL(*scan);
		REQUIRE(lookup->Fetch<int64_t>(0, 0) == scan->Fetch<int64_t>(0, 0));
	}
}
