#include "catch.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

//! Runs the index statement and, after a delay, the insert on another connection, and returns whether each succeeded
void RunConcurrently(DuckDB &db, const string &index_sql, const string &insert_sql, idx_t delay_ms, bool &index_ok,
                     bool &insert_ok) {
	Connection index_con(db);
	Connection insert_con(db);
	std::atomic<bool> started {false};
	std::thread index_thread([&]() {
		started = true;
		index_ok = !index_con.Query(index_sql)->HasError();
	});
	while (!started) {
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
	insert_ok = !insert_con.Query(insert_sql)->HasError();
	index_thread.join();
}

} // namespace

TEST_CASE("Rows appended while an index is built are not missing from it", "[storage]") {
	DuckDB db(nullptr);
	Connection con(db);
	for (idx_t attempt = 0; attempt < 10; attempt++) {
		auto delay_ms = attempt * 5;
		REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE t (id INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT i FROM range(1, 1000001) tbl(i)"));

		// a NULL in the key column of a primary key
		bool index_ok, insert_ok;
		RunConcurrently(db, "ALTER TABLE t ADD PRIMARY KEY (id)", "INSERT INTO t VALUES (NULL)", delay_ms, index_ok,
		                insert_ok);
		REQUIRE(!(index_ok && insert_ok));

		// a duplicate key
		REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE t (id INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT i FROM range(1, 1000001) tbl(i)"));
		RunConcurrently(db, "CREATE UNIQUE INDEX idx ON t (id)", "INSERT INTO t VALUES (1)", delay_ms, index_ok,
		                insert_ok);
		REQUIRE(!(index_ok && insert_ok));

		// a row that an index lookup has to find
		REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE t (id INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT i FROM range(1, 1000001) tbl(i)"));
		RunConcurrently(db, "CREATE INDEX idx ON t (id)", "INSERT INTO t VALUES (2000000)", delay_ms, index_ok,
		                insert_ok);
		if (index_ok && insert_ok) {
			auto result = con.Query("SELECT count(*) FROM t WHERE id = 2000000");
			REQUIRE(CHECK_COLUMN(result, 0, {1}));
		}
	}
}
