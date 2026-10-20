#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

using namespace duckdb;

static void IdentityFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(args.data[0]);
}

// A function with the name and the other callbacks of lower() but another implementation
TEST_CASE("Filters through a function that only looks like lower() do not prune duckdb_tables", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE SCHEMA \"MiXeD\""));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE \"MiXeD\".t (i INTEGER)"));

	auto function = LowerFun::GetFunction();
	function.SetFunctionCallback(IdentityFunction);
	CreateScalarFunctionInfo info(function);
	info.internal = false;
	con.BeginTransaction();
	auto &catalog = Catalog::GetCatalog(*con.context, Identifier("memory"));
	MetaTransaction::Get(*con.context)
	    .ModifyDatabase(catalog.GetAttached(), DatabaseModificationType::CREATE_CATALOG_ENTRY);
	catalog.CreateFunction(*con.context, info);
	con.Commit();

	auto result = con.Query("SELECT lower('MiXeD')");
	REQUIRE(CHECK_COLUMN(result, 0, {"MiXeD"}));
	result = con.Query("SELECT table_name FROM duckdb_tables() WHERE lower(schema_name) = 'MiXeD'");
	REQUIRE(CHECK_COLUMN(result, 0, {"t"}));
	result = con.Query("SELECT table_name FROM duckdb_tables() WHERE system.main.lower(schema_name) = 'mixed'");
	REQUIRE(CHECK_COLUMN(result, 0, {"t"}));
}
