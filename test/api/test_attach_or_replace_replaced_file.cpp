#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/file_system.hpp"

using namespace duckdb;

// ATTACH OR REPLACE of the attached path reads a file that was replaced on disk (duckdb/duckdb#20748)

#ifndef _WIN32
TEST_CASE("Test ATTACH OR REPLACE of a database file that was replaced", "[api]") {
	auto path = TestCreatePath("or_replace_replaced_file.db");
	auto new_path = TestCreatePath("or_replace_replaced_file_new.db");
	DeleteDatabase(path);
	DeleteDatabase(new_path);
	DuckDB db(nullptr);
	Connection con(db);
	auto &fs = FileSystem::GetFileSystem(*con.context);

	for (auto read_only : {true, false}) {
		auto mode = read_only ? string(" (READ_ONLY)") : string();
		REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS w"));
		REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE w.t AS SELECT 1 AS i"));
		REQUIRE_NO_FAIL(con.Query("DETACH w"));
		REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS r" + mode));
		auto result = con.Query("SELECT i FROM r.t");
		REQUIRE(CHECK_COLUMN(result, 0, {1}));

		// a writer replaces the file with a new version
		REQUIRE_NO_FAIL(con.Query("ATTACH '" + new_path + "' AS w"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE w.t AS SELECT 2 AS i, 'new' AS s"));
		REQUIRE_NO_FAIL(con.Query("DETACH w"));
		fs.MoveFile(new_path, path);

		REQUIRE_NO_FAIL(con.Query("ATTACH OR REPLACE '" + path + "' AS r" + mode));
		result = con.Query("SELECT i, s FROM r.t");
		REQUIRE(CHECK_COLUMN(result, 0, {2}));
		REQUIRE(CHECK_COLUMN(result, 1, {"new"}));
		REQUIRE_NO_FAIL(con.Query("DETACH r"));
	}
}
#endif
