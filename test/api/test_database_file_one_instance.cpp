#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/db_instance_cache.hpp"

#include <thread>

using namespace duckdb;

//! The error of an open that another instance of the process prevents, on Windows the file is not shared
static const char *const OPEN_CONFLICT =
#ifdef _WIN32
    "";
#else
    "Unique file handle conflict";
#endif

TEST_CASE("Two database instances of a process do not open the same file for writing", "[api]") {
	auto path = TestCreatePath("one_instance_per_file.db");
	DBConfig read_only;
	read_only.options.access_mode = AccessMode::READ_ONLY;
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT 42 AS i"));

		// a second instance keeps its own header, free list and WAL, and overwrites the blocks of the first
		REQUIRE_THROWS_WITH(make_uniq<DuckDB>(path), Catch::Contains(OPEN_CONFLICT));
		REQUIRE_THROWS_WITH(make_uniq<DuckDB>(path, &read_only), Catch::Contains(OPEN_CONFLICT));
		DBInstanceCache instance_cache;
		DBConfig config;
		REQUIRE_THROWS_WITH(instance_cache.GetOrCreateInstance(path, config, true), Catch::Contains(OPEN_CONFLICT));
		DuckDB other(nullptr);
		Connection other_con(other);
		auto result = other_con.Query("ATTACH '" + path + "' AS other_db");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), OPEN_CONFLICT));

		// the first instance is unaffected
		result = con.Query("SELECT i FROM t");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
	// once that instance is closed, another one opens the file
	{
		DuckDB db(path);
		Connection con(db);
		auto result = con.Query("SELECT i FROM t");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
	// instances that only read share the file
	{
		DuckDB db(path, &read_only);
		DuckDB second_db(path, &read_only);
		Connection con(second_db);
		auto result = con.Query("SELECT i FROM t");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
}

TEST_CASE("A failed attach of a file that another instance reads can be retried", "[api]") {
	auto path = TestCreatePath("one_instance_retry.db");
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT 42 AS i"));
	}
	DBConfig read_only;
	read_only.options.access_mode = AccessMode::READ_ONLY;
	DuckDB reader(path, &read_only);
	DuckDB other(nullptr);
	Connection con(other);
	// the other instance reads it under another name too
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS other_name (READ_ONLY)"));
	// and under the name the reader has for it
	REQUIRE_FAIL(
	    con.Query("ATTACH IF NOT EXISTS '" + path + "' AS one_instance_retry (READ_ONLY, ENCRYPTION_KEY 'abc')"));
	REQUIRE_NO_FAIL(con.Query("ATTACH IF NOT EXISTS '" + path + "' AS one_instance_retry (READ_ONLY)"));
	auto result = con.Query("SELECT i FROM one_instance_retry.t");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
}

TEST_CASE("Connections attach a file that another instance reads under another name at the same time", "[api]") {
	auto path = TestCreatePath("one_instance_alias.db");
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT 42 AS i"));
	}
	DBConfig read_only;
	read_only.options.access_mode = AccessMode::READ_ONLY;
	DuckDB reader(path, &read_only);
	DuckDB other(nullptr);
	Connection con(other);
	for (idx_t round = 0; round < 20; round++) {
		mutex error_lock;
		vector<string> errors;
		auto attach = [&]() {
			Connection attach_con(other);
			auto result = attach_con.Query("ATTACH IF NOT EXISTS '" + path + "' AS alias (READ_ONLY)");
			if (result->HasError()) {
				lock_guard<mutex> guard(error_lock);
				errors.push_back(result->GetError());
			}
		};
		std::thread first(attach);
		std::thread second(attach);
		first.join();
		second.join();
		INFO((errors.empty() ? string() : errors[0]));
		REQUIRE(errors.empty());
		REQUIRE_NO_FAIL(con.Query("DETACH alias"));
	}
}

TEST_CASE("Database instances attach a file of another type at the same time", "[api]") {
	auto path = TestCreatePath("one_instance_concurrent.csv");
	{
		DuckDB db(nullptr);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("COPY (SELECT 42 AS i) TO '" + path + "'"));
	}
	mutex error_lock;
	vector<string> errors;
	auto attach_repeatedly = [&]() {
		DuckDB db(nullptr);
		Connection con(db);
		for (idx_t i = 0; i < 50; i++) {
			auto result = con.Query("ATTACH '" + path + "' AS p");
			if (!result->HasError()) {
				result = con.Query("DETACH p");
			}
			if (result->HasError()) {
				lock_guard<mutex> guard(error_lock);
				errors.push_back(result->GetError());
				return;
			}
		}
	};
	std::thread first(attach_repeatedly);
	std::thread second(attach_repeatedly);
	first.join();
	second.join();
	INFO((errors.empty() ? string() : errors[0]));
	REQUIRE(errors.empty());
}
