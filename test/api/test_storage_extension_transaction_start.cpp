#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

// A transaction manager may need the transactions of other databases while it starts one (duckdb/duckdb#22258)

namespace {

enum class StartLookup { SECRET_LOOKUP, SYSTEM_TRANSACTION, OWN_TRANSACTION };

class LookupTransactionManager : public DuckTransactionManager {
public:
	LookupTransactionManager(AttachedDatabase &db, StartLookup lookup) : DuckTransactionManager(db), lookup(lookup) {
	}

public:
	Transaction &StartTransaction(ClientContext &context) override {
		switch (lookup) {
		case StartLookup::SECRET_LOOKUP: {
			// the secret lookup that httpfs does before an HTTP request
			ClientContextFileOpener opener(context);
			FileOpenerInfo info;
			info.file_path = "https://example.com/version";
			KeyValueSecretReader reader(opener, &info, "http");
			break;
		}
		case StartLookup::SYSTEM_TRANSACTION:
			(void)CatalogTransaction::GetSystemCatalogTransaction(context);
			break;
		case StartLookup::OWN_TRANSACTION:
			(void)Transaction::Get(context, db);
			break;
		}
		return DuckTransactionManager::StartTransaction(context);
	}

private:
	StartLookup lookup;
};

struct LookupStorageExtension : StorageExtension {
	explicit LookupStorageExtension(StartLookup lookup) {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq<DuckCatalog>(db);
		};
		storage_info = make_shared_ptr<LookupInfo>(lookup);
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo> info, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<LookupTransactionManager>(db, dynamic_cast<LookupInfo &>(*info).lookup);
		};
	}

	struct LookupInfo : StorageExtensionInfo {
		explicit LookupInfo(StartLookup lookup) : lookup(lookup) {
		}
		StartLookup lookup;
	};
};

} // namespace

TEST_CASE("Test a transaction manager that needs other transactions while it starts one", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "secret_lookup",
	                           make_shared_ptr<LookupStorageExtension>(StartLookup::SECRET_LOOKUP));
	StorageExtension::Register(config, "system_lookup",
	                           make_shared_ptr<LookupStorageExtension>(StartLookup::SYSTEM_TRANSACTION));
	StorageExtension::Register(config, "own_lookup",
	                           make_shared_ptr<LookupStorageExtension>(StartLookup::OWN_TRANSACTION));
	DuckDB db(nullptr, &config);
	Connection con(db);

	for (auto type : {"secret_lookup", "system_lookup"}) {
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("ATTACH ':memory:' AS %s (TYPE %s)", type, type)));
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("CREATE TABLE %s.t AS SELECT 42 AS i", type)));
		auto result = con.Query(StringUtil::Format("SELECT i FROM %s.t", type));
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
		// an explicit transaction that starts the transaction of the database in its middle
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		REQUIRE_NO_FAIL(con.Query("SELECT 1"));
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("INSERT INTO %s.t VALUES (43)", type)));
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
		result = con.Query(StringUtil::Format("SELECT count(*) FROM %s.t", type));
		REQUIRE(CHECK_COLUMN(result, 0, {2}));
	}

	// the transaction that is being started cannot be used yet
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS own_lookup (TYPE own_lookup)"));
	auto result = con.Query("CREATE TABLE own_lookup.t AS SELECT 42 AS i");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "while it is being started"));
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
}
