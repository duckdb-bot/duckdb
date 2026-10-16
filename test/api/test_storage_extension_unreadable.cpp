#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"

using namespace duckdb;

// A database that cannot start a transaction, as one whose files were deleted
static atomic<ExceptionType> unreadable_error {ExceptionType::INVALID};
// Whether it lists its schemas without a transaction, as the SQLite catalog does, so that only reading its entries
// fails
static atomic<bool> unreadable_lists_schemas {false};

class UnreadableTransactionManager : public DuckTransactionManager {
public:
	explicit UnreadableTransactionManager(AttachedDatabase &db) : DuckTransactionManager(db) {
	}

	Transaction &StartTransaction(ClientContext &context) override {
		switch (unreadable_error.load()) {
		case ExceptionType::IO:
			throw IOException("Unable to open the unreadable database");
		case ExceptionType::INTERRUPT:
			throw InterruptException();
		case ExceptionType::INTERNAL:
			throw InternalException("Unable to open the unreadable database");
		case ExceptionType::FATAL:
			throw FatalException("Unable to open the unreadable database");
		default:
			return DuckTransactionManager::StartTransaction(context);
		}
	}
};

class UnreadableCatalog : public DuckCatalog {
public:
	explicit UnreadableCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override {
		if (!unreadable_lists_schemas) {
			DuckCatalog::ScanSchemas(context, callback);
			return;
		}
		DuckCatalog::ScanSchemas(VisibilityBound::AllCommitted(), [&](DuckSchemaEntry &schema) { callback(schema); });
	}
};

struct UnreadableStorageExtension : StorageExtension {
	UnreadableStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq_base<Catalog, UnreadableCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<UnreadableTransactionManager>(db);
		};
	}
};

static unique_ptr<DuckDB> OpenWithUnreadableDatabase() {
	DBConfig config;
	StorageExtension::Register(config, "unreadable", make_shared_ptr<UnreadableStorageExtension>());
	auto db = make_uniq<DuckDB>(nullptr, &config);
	Connection con(*db);
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS broken (TYPE unreadable)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE broken.nonexistent_table AS SELECT 1 AS i"));
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS healthy"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE healthy.nonexistent_tbl AS SELECT 1 AS i"));
	return db;
}

TEST_CASE("A missing entry error leaves out the suggestions of a database that fails to be read", "[api]") {
	auto db = OpenWithUnreadableDatabase();
	Connection con(*db);

	for (bool lists_schemas : {false, true}) {
		// reading the schemas fails, or only reading the entries of a schema does
		unreadable_lists_schemas = lists_schemas;
		unreadable_error = ExceptionType::IO;

		// the error is about the missing table, and the readable database still gives its suggestion
		auto result = con.Query("SELECT * FROM memory.main.nonexistent");
		REQUIRE(result->HasError());
		REQUIRE(result->GetErrorType() == ExceptionType::CATALOG);
		REQUIRE(StringUtil::Contains(result->GetError(), "Table with name nonexistent does not exist"));
		result = con.Query("SELECT * FROM nonexistent");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "Table with name nonexistent does not exist"));
		REQUIRE(StringUtil::Contains(result->GetError(), "healthy.nonexistent_tbl"));
		result = con.Query("SELECT nonexistent_function()");
		REQUIRE(result->HasError());
		REQUIRE(
		    StringUtil::Contains(result->GetError(), "Scalar Function with name nonexistent_function does not exist"));
		REQUIRE_NO_FAIL(con.Query("SELECT 42"));

		// the database itself still reports its error
		result = con.Query("SELECT * FROM broken.nonexistent_table");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "Unable to open the unreadable database"));

		// an interrupt or an internal error is not left out
		unreadable_error = ExceptionType::INTERRUPT;
		result = con.Query("SELECT * FROM nonexistent");
		REQUIRE(result->HasError());
		REQUIRE(result->GetErrorType() == ExceptionType::INTERRUPT);
		unreadable_error = ExceptionType::INTERNAL;
		result = con.Query("SELECT * FROM nonexistent");
		REQUIRE(result->HasError());
		REQUIRE(result->GetErrorType() == ExceptionType::INTERNAL);
		unreadable_error = ExceptionType::INVALID;

		// once it can be read, it gives its suggestions again
		result = con.Query("SELECT * FROM nonexistent_table");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "broken.nonexistent_table"));
	}
	unreadable_lists_schemas = false;
}

TEST_CASE("A fatal error of a database read for suggestions invalidates the database", "[api]") {
	auto db = OpenWithUnreadableDatabase();
	Connection con(*db);

	unreadable_error = ExceptionType::FATAL;
	auto result = con.Query("SELECT * FROM nonexistent");
	unreadable_error = ExceptionType::INVALID;
	REQUIRE(result->HasError());
	REQUIRE(result->GetErrorType() == ExceptionType::FATAL);
	REQUIRE(con.Query("SELECT 1")->HasError());
}
