#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test list searches over VARIANT values when list_transform is not available", "[api]") {
	DBConfig config;
	config.options.load_extensions = false;
	config.SetOptionByName("autoload_known_extensions", false);
	DuckDB db(nullptr, &config);
	Connection con(db);

	auto result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'list_transform'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));

	// the list elements cannot be collated, so the searched value is not collated either
	result = con.Query("SELECT list_contains('[1]'::INTEGER[]::VARIANT[], 1::VARIANT), "
	                   "list_position('[3, 1]'::INTEGER[]::VARIANT[], 1::VARIANT)");
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	REQUIRE(CHECK_COLUMN(result, 1, {2}));
}
