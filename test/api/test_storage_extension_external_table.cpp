#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

// Checkpoints of an in-memory DuckCatalog skip the table entries that an extension owns (duckdb/duckdb#24233)

namespace {

// an entry may also ask to be bound like a DuckDB table without being a DuckTableEntry
class ExternalTableEntry : public TableCatalogEntry {
public:
	ExternalTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
	    : TableCatalogEntry(catalog, schema, info), columns(info.columns.Copy()),
	      duck_binding(StringUtil::StartsWith(info.GetTableName().GetIdentifierName(), "ext_binding")) {
	}

public:
	bool IsDuckTable() const override {
		return duck_binding;
	}
	const ColumnList &GetColumns() const override {
		return columns;
	}
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &, column_t) override {
		return nullptr;
	}
	TableFunction GetScanFunction(ClientContext &, unique_ptr<FunctionData> &) override {
		throw NotImplementedException("external tables cannot be scanned");
	}
	TableStorageInfo GetStorageInfo(ClientContext &) override {
		return {};
	}

private:
	ColumnList columns;
	bool duck_binding;
};

// tables whose name starts with "ext" are owned by the extension, other tables are DuckDB tables
class ExternalSchemaEntry : public DuckSchemaEntry {
public:
	ExternalSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : DuckSchemaEntry(catalog, info) {
	}

public:
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override {
		if (!StringUtil::StartsWith(info.Base().GetTableName().GetIdentifierName(), "ext")) {
			return DuckSchemaEntry::CreateTable(transaction, info);
		}
		auto table = make_uniq<ExternalTableEntry>(catalog, *this, info.Base());
		return AddEntry(transaction, std::move(table), info.Base().on_conflict);
	}
};

class ExternalCatalog : public DuckCatalog {
public:
	explicit ExternalCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

public:
	void ReplaceDefaultSchema(CatalogTransaction transaction) {
		auto &schemas = GetSchemaCatalogSet();
		(void)schemas.DropEntry(transaction, DEFAULT_SCHEMA, true, true);
		CreateSchemaInfo info;
		info.SetQualifiedName(QualifiedName({Identifier::DefaultSchema()}, Identifier()));
		info.internal = true;
		LogicalDependencyList dependencies;
		auto schema = make_uniq<ExternalSchemaEntry>(*this, info);
		if (!schemas.CreateEntry(transaction, DEFAULT_SCHEMA, std::move(schema), dependencies)) {
			throw InternalException("could not replace the main schema");
		}
	}
};

struct ExternalStorageExtension : StorageExtension {
	ExternalStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			auto catalog = make_uniq<ExternalCatalog>(db);
			catalog->Initialize(false);
			catalog->ReplaceDefaultSchema(CatalogTransaction::GetSystemTransaction(db.GetDatabase()));
			return std::move(catalog);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

} // namespace

TEST_CASE("Test checkpoints of an in-memory catalog with tables owned by an extension", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "external", make_shared_ptr<ExternalStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS ext (TYPE external)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE ext.ext_tbl (i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE ext.ext_binding (i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE ext.duck_tbl AS SELECT i FROM range(10000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("DELETE FROM ext.duck_tbl WHERE i % 2 = 0"));
	REQUIRE_NO_FAIL(con.Query("FORCE CHECKPOINT ext"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE ext.ext_tbl2 (i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT ext"));

	auto result = con.Query("SELECT count(*), sum(i) FROM ext.duck_tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {5000}));
	REQUIRE(CHECK_COLUMN(result, 1, {25000000}));

	// every commit that writes to the WAL checkpoints
	REQUIRE_NO_FAIL(con.Query("SET checkpoint_threshold = '1B'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE ext.ext_tbl3 (i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO ext.duck_tbl VALUES (10001)"));

	result = con.Query("SELECT count(*), sum(i) FROM ext.duck_tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {5001}));
	REQUIRE(CHECK_COLUMN(result, 1, {25010001}));
	result = con.Query("SELECT table_name FROM duckdb_tables() WHERE database_name = 'ext' ORDER BY table_name");
	REQUIRE(CHECK_COLUMN(result, 0, {"duck_tbl", "ext_binding", "ext_tbl", "ext_tbl2", "ext_tbl3"}));
}
