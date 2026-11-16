#include "catch.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

//! Runs the index statement and, after a delay, the update on another connection, and returns their errors
void RunConcurrently(DuckDB &db, const string &index_sql, const string &update_sql, idx_t delay_ms, string &index_error,
                     string &update_error) {
	Connection index_con(db);
	Connection update_con(db);
	std::atomic<bool> started {false};
	std::thread index_thread([&]() {
		started = true;
		auto result = index_con.Query(index_sql);
		index_error = result->HasError() ? result->GetError() : string();
	});
	while (!started) {
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
	auto result = update_con.Query(update_sql);
	update_error = result->HasError() ? result->GetError() : string();
	index_thread.join();
}

//! A failure is a transaction conflict, an update the index build cannot see yet, or the constraint of the index
bool IsExpectedError(const string &error) {
	return error.empty() || StringUtil::Contains(error, "Transaction conflict") ||
	       StringUtil::Contains(error, "another transaction has altered this table") ||
	       StringUtil::Contains(error, "Cannot create index with outstanding updates") ||
	       StringUtil::Contains(error, "Duplicate key") || StringUtil::Contains(error, "Data contains duplicates");
}

//! Checks that an index lookup of the key finds the rows a full scan finds
void VerifyIndexLookup(Connection &con, int32_t key) {
	auto lookup = con.Query("SELECT count(*) FROM t WHERE id = " + to_string(key));
	auto scan = con.Query("SELECT count(*) FROM t WHERE id::VARCHAR = '" + to_string(key) + "'");
	REQUIRE_NO_FAIL(*lookup);
	REQUIRE_NO_FAIL(*scan);
	INFO("key " << key);
	REQUIRE(lookup->Collection().GetValue(0, 0) == scan->Collection().GetValue(0, 0));
}

} // namespace

TEST_CASE("An update made while an index is built is not missing from it", "[storage]") {
	DuckDB db(nullptr);
	Connection con(db);
	struct Scenario {
		string index_sql;
		string update_sql;
		//! The keys the update changes
		vector<int32_t> keys;
		//! Whether both statements cannot succeed
		bool conflicts;
	};
	vector<Scenario> scenarios {
	    {"CREATE INDEX idx ON t (id)", "UPDATE t SET id = 30000000 WHERE id = 5", {5, 30000000}, false},
	    {"CREATE UNIQUE INDEX idx ON t (id)", "UPDATE t SET id = 6 WHERE id = 5", {5, 6}, true},
	    {"ALTER TABLE t ADD PRIMARY KEY (id)", "UPDATE t SET id = 30000000 WHERE id = 5", {5, 30000000}, false}};
	for (idx_t attempt = 0; attempt < 10; attempt++) {
		auto delay_ms = attempt * 5;
		for (auto &scenario : scenarios) {
			// the key is not the first column
			REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE t (pad VARCHAR, id INTEGER)"));
			REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT 'x', i FROM range(1, 1000001) tbl(i)"));
			string index_error, update_error;
			RunConcurrently(db, scenario.index_sql, scenario.update_sql, delay_ms, index_error, update_error);
			INFO(scenario.index_sql << " | " << scenario.update_sql << " | delay " << delay_ms
			                        << " | index error: " << index_error << " | update error: " << update_error);
			REQUIRE(IsExpectedError(index_error));
			REQUIRE(IsExpectedError(update_error));
			if (scenario.conflicts) {
				REQUIRE((!index_error.empty() || !update_error.empty()));
			}
			if (index_error.empty()) {
				for (auto key : scenario.keys) {
					VerifyIndexLookup(con, key);
				}
			}
		}
	}
}
