#include "catch.hpp"
#include "duckdb/main/database.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test that indexes on list_sort serve connections other than the one that bound them", "[api]") {
	auto path = TestCreatePath("list_sort_index_connections.db");
	DeleteDatabase(path);
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (id INTEGER, l INTEGER[])"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1, [3, 1, 2])"));
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX i ON t ((list_sort(l)[1]))"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE u (id INTEGER, l INTEGER[])"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO u VALUES (1, [3, 1, 2])"));
		REQUIRE_NO_FAIL(con.Query("CREATE UNIQUE INDEX ui ON u ((list_sort(l)[1]))"));
	}

	// the indexes are read from disk and bound by the first connection that writes to the tables, which then closes
	DuckDB db(path);
	{
		Connection con1(db);
		REQUIRE_NO_FAIL(con1.Query("INSERT INTO t VALUES (2, [9, 8])"));
		REQUIRE_NO_FAIL(con1.Query("INSERT INTO u VALUES (2, [9, 8])"));
	}

	// the key expressions run for the appends, unique checks and deletes of another connection
	Connection con2(db);
	REQUIRE_NO_FAIL(con2.Query("INSERT INTO t VALUES (3, [3, 4])"));
	REQUIRE_NO_FAIL(con2.Query("INSERT INTO u VALUES (3, [3, 4])"));
	// the smallest element of [1, 5] is that of the first row
	REQUIRE_FAIL(con2.Query("INSERT INTO u VALUES (4, [1, 5])"));
	REQUIRE_NO_FAIL(con2.Query("DELETE FROM t WHERE id = 1"));
	REQUIRE_NO_FAIL(con2.Query("DELETE FROM u WHERE id = 1"));
	REQUIRE_NO_FAIL(con2.Query("INSERT INTO u VALUES (4, [1, 5])"));
	REQUIRE_NO_FAIL(con2.Query("UPDATE t SET l = [7, 0] WHERE id = 2"));

	auto result = con2.Query("SELECT id FROM t WHERE list_sort(l)[1] = 0");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	result = con2.Query("SELECT id FROM t ORDER BY id");
	REQUIRE(CHECK_COLUMN(result, 0, {2, 3}));
	result = con2.Query("SELECT id FROM u WHERE list_sort(l)[1] = 1");
	REQUIRE(CHECK_COLUMN(result, 0, {4}));
	result = con2.Query("SELECT id FROM u ORDER BY id");
	REQUIRE(CHECK_COLUMN(result, 0, {2, 3, 4}));
}
