#include "catch.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
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

	// a reader with an older snapshot defers the index cleanup of a delete until it finishes, and the cleanup then
	// runs without a connection
	Connection reader(db);
	REQUIRE_NO_FAIL(reader.Query("BEGIN"));
	result = reader.Query("SELECT id FROM u WHERE list_sort(l)[1] = 8");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	REQUIRE_NO_FAIL(con2.Query("DELETE FROM u WHERE id = 2"));
	result = reader.Query("SELECT id FROM u WHERE list_sort(l)[1] = 8");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	REQUIRE_NO_FAIL(reader.Query("COMMIT"));
	REQUIRE_NO_FAIL(con2.Query("INSERT INTO u VALUES (5, [8, 9])"));
	result = con2.Query("SELECT id FROM u WHERE list_sort(l)[1] = 8");
	REQUIRE(CHECK_COLUMN(result, 0, {5}));

	// a key that another connection commits first is found when the transaction commits, which reverts the appends
	// of its earlier chunks to the index
	Connection con3(db);
	REQUIRE_NO_FAIL(con2.Query("BEGIN"));
	REQUIRE_NO_FAIL(con2.Query("INSERT INTO u SELECT 1000 + i, [1000 + i, 2000 + i] FROM range(3000) r(i)"));
	REQUIRE_NO_FAIL(con2.Query("INSERT INTO u VALUES (6, [20, 30])"));
	REQUIRE_NO_FAIL(con3.Query("INSERT INTO u VALUES (7, [20, 40])"));
	auto commit = con2.Query("COMMIT");
	REQUIRE(commit->HasError());
	REQUIRE(StringUtil::Contains(StringUtil::Lower(commit->GetError()), "duplicate key"));
	// the keys of the reverted rows are free again
	REQUIRE_NO_FAIL(con2.Query("INSERT INTO u VALUES (8, [1000, 5000])"));
	result = con2.Query("SELECT id FROM u WHERE list_sort(l)[1] = 20");
	REQUIRE(CHECK_COLUMN(result, 0, {7}));
	result = con2.Query("SELECT id FROM u WHERE list_sort(l)[1] = 1000");
	REQUIRE(CHECK_COLUMN(result, 0, {8}));
	result = con2.Query("SELECT id FROM u ORDER BY id");
	REQUIRE(CHECK_COLUMN(result, 0, {3, 4, 5, 7, 8}));
}

TEST_CASE("Test that the checkpoint on shutdown remaps an index on list_sort", "[api]") {
	auto path = TestCreatePath("list_sort_index_shutdown.db");
	DeleteDatabase(path);
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (id INTEGER, l INTEGER[])"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT i, [i + 1, i] FROM range(250000) r(i)"));
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX i ON t ((list_sort(l)[1]))"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		// the deletes make the checkpoint on shutdown merge row groups, which remaps the row ids of the index after
		// the connection that bound it has closed
		REQUIRE_NO_FAIL(con.Query("DELETE FROM t WHERE id % 4 != 0"));
	}

	// the checkpoint on shutdown succeeded, so the write ahead log is gone
	LocalFileSystem fs;
	REQUIRE(!fs.FileExists(path + ".wal"));

	DuckDB db(path);
	Connection con(db);
	auto result = con.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(result, 0, {62500}));
	result = con.Query("SELECT id FROM t WHERE list_sort(l)[1] = 8");
	REQUIRE(CHECK_COLUMN(result, 0, {8}));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (250000, [250001, 250000])"));
	result = con.Query("SELECT id FROM t WHERE list_sort(l)[1] = 250000");
	REQUIRE(CHECK_COLUMN(result, 0, {250000}));
}
