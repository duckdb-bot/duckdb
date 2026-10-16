#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"

using namespace duckdb;

// A database that cannot start a transaction, as one whose files were deleted
static atomic<bool> unreadable_database_fails {false};

class UnreadableTransactionManager : public DuckTransactionManager {
public:
	explicit UnreadableTransactionManager(AttachedDatabase &db) : DuckTransactionManager(db) {
	}

	Transaction &StartTransaction(ClientContext &context) override {
		if (unreadable_database_fails) {
			throw IOException("Unable to open the unreadable database");
		}
		return DuckTransactionManager::StartTransaction(context);
	}
};

struct UnreadableStorageExtension : StorageExtension {
	UnreadableStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq_base<Catalog, DuckCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<UnreadableTransactionManager>(db);
		};
	}
};

TEST_CASE("A missing entry error does not read databases that fail to start a transaction", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "unreadable", make_shared_ptr<UnreadableStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS broken (TYPE unreadable)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE broken.nonexistent_table AS SELECT 1 AS i"));
	unreadable_database_fails = true;

	// the error is about the missing table, in the database that the query names and in the search path
	auto result = con.Query("SELECT * FROM memory.main.nonexistent");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Table with name nonexistent does not exist"));
	result = con.Query("SELECT * FROM nonexistent");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Table with name nonexistent does not exist"));
	result = con.Query("SELECT nonexistent_function()");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Scalar Function with name nonexistent_function does not exist"));
	// the connection keeps working
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));

	// a readable database still gives its suggestions
	unreadable_database_fails = false;
	result = con.Query("SELECT * FROM nonexistent");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "broken.nonexistent_table"));

	// the database itself still reports its error
	unreadable_database_fails = true;
	result = con.Query("SELECT * FROM broken.nonexistent_table");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Unable to open the unreadable database"));
	unreadable_database_fails = false;
}
