#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test MAP and UNION comparisons when the functions that collate them are not available", "[api]") {
	DBConfig config;
	config.options.load_extensions = false;
	config.SetOptionByName("autoload_known_extensions", false);
	DuckDB db(nullptr, &config);
	Connection con(db);

	auto result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'map_entries'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));

	// the contents cannot be collated, so the values compare as they are stored, as before
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE m (m MAP(INTEGER, INTERVAL), u UNION(i INTERVAL))"));
	result = con.Query("SELECT count(*) FROM m WHERE m = m AND u = u");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	result = con.Query("SELECT count(DISTINCT m), count(DISTINCT u) FROM m");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE(CHECK_COLUMN(result, 1, {0}));
}
