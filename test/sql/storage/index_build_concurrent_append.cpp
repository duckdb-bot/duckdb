#include "catch.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

//! Runs the index statement and, after a delay, the insert on another connection, and returns their errors
void RunConcurrently(DuckDB &db, const string &index_sql, const string &insert_sql, idx_t delay_ms, string &index_error,
                     string &insert_error) {
	Connection index_con(db);
	Connection insert_con(db);
	std::atomic<bool> started {false};
	std::thread index_thread([&]() {
		started = true;
		auto result = index_con.Query(index_sql);
		index_error = result->HasError() ? result->GetError() : string();
	});
	while (!started) {
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
	auto result = insert_con.Query(insert_sql);
	insert_error = result->HasError() ? result->GetError() : string();
	index_thread.join();
}

//! A failure is a transaction conflict, or the constraint the other statement violates
bool IsExpectedError(const string &error) {
	return error.empty() || StringUtil::Contains(error, "Transaction conflict") ||
	       StringUtil::Contains(error, "another transaction has altered this table") ||
	       StringUtil::Contains(error, "NOT NULL constraint failed") ||
	       StringUtil::Contains(error, "Data contains duplicates") || StringUtil::Contains(error, "Duplicate key");
}

void CreateTable(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE t (id INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT i FROM range(1, 1000001) tbl(i)"));
}

} // namespace

TEST_CASE("Rows appended while an index is built are not missing from it", "[storage]") {
	DuckDB db(nullptr);
	Connection con(db);
	struct Scenario {
		string index_sql;
		string insert_sql;
	};
	vector<Scenario> scenarios {{"ALTER TABLE t ADD PRIMARY KEY (id)", "INSERT INTO t VALUES (NULL)"},
	                            {"ALTER TABLE t ADD PRIMARY KEY (id)", "INSERT INTO t VALUES (1)"},
	                            {"ALTER TABLE t ADD UNIQUE (id)", "INSERT INTO t VALUES (1)"},
	                            {"CREATE UNIQUE INDEX idx ON t (id)", "INSERT INTO t VALUES (1)"}};
	for (idx_t attempt = 0; attempt < 10; attempt++) {
		auto delay_ms = attempt * 5;
		for (auto &scenario : scenarios) {
			CreateTable(con);
			string index_error, insert_error;
			RunConcurrently(db, scenario.index_sql, scenario.insert_sql, delay_ms, index_error, insert_error);
			INFO(scenario.index_sql << " | " << scenario.insert_sql << " | delay " << delay_ms
			                        << " | index error: " << index_error << " | insert error: " << insert_error);
			REQUIRE(IsExpectedError(index_error));
			REQUIRE(IsExpectedError(insert_error));
			// the key would be NULL or duplicate if both succeeded
			REQUIRE(!(index_error.empty() && insert_error.empty()));
		}

		// a row that an index lookup has to find
		CreateTable(con);
		string index_error, insert_error;
		RunConcurrently(db, "CREATE INDEX idx ON t (id)", "INSERT INTO t VALUES (2000000)", delay_ms, index_error,
		                insert_error);
		REQUIRE(IsExpectedError(index_error));
		REQUIRE(IsExpectedError(insert_error));
		if (index_error.empty() && insert_error.empty()) {
			auto result = con.Query("SELECT count(*) FROM t WHERE id = 2000000");
			REQUIRE(CHECK_COLUMN(result, 0, {1}));
		}
	}
}
