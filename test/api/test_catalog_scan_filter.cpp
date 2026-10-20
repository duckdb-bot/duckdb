#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

using namespace duckdb;

static void IdentityFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(args.data[0]);
}

// A function with the name and the other callbacks of lower() but another implementation
TEST_CASE("Filters through a function that only looks like lower() keep the tables it matches", "[api]") {
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

static atomic<idx_t> schema_scans {0};

// Counts how often the schemas are listed, which is a remote request for catalogs such as iceberg
class CountingCatalog : public DuckCatalog {
public:
	explicit CountingCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override {
		schema_scans++;
		DuckCatalog::ScanSchemas(context, std::move(callback));
	}
};

struct CountingStorageExtension : StorageExtension {
	CountingStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq_base<Catalog, CountingCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

TEST_CASE("Filters on the database name do not list the schemas of other catalogs", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "counting", make_shared_ptr<CountingStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS counted (TYPE counting)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE counted.t (i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE mine (i INTEGER)"));

	schema_scans = 0;
	auto result = con.Query("SELECT table_name FROM duckdb_tables() WHERE database_name = 'memory'");
	REQUIRE(CHECK_COLUMN(result, 0, {"mine"}));
	result = con.Query("SHOW TABLES FROM memory.main");
	REQUIRE(CHECK_COLUMN(result, 0, {"mine"}));
	REQUIRE(schema_scans == 0);

	result = con.Query("SELECT table_name FROM duckdb_tables() WHERE database_name = 'counted'");
	REQUIRE(CHECK_COLUMN(result, 0, {"t"}));
	REQUIRE(schema_scans > 0);
}
