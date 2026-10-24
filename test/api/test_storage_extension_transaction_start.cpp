#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

#include <chrono>
#include <thread>

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

struct ConcurrentStartState {
	bool fail_first_start = false;
	atomic<idx_t> starts {0};
	atomic<idx_t> commits {0};
	atomic<idx_t> next_slot {0};
	mutex gate_lock;
	std::condition_variable gate;
	idx_t gate_arrivals = 0;

	void Reset(bool fail_first) {
		fail_first_start = fail_first;
		starts = 0;
		commits = 0;
		next_slot = 0;
		gate_arrivals = 0;
	}

	//! Lets two workers ask for the transaction at the same time, if the scheduler runs two
	void WaitForOtherWorker() {
		unique_lock<mutex> guard(gate_lock);
		gate_arrivals++;
		gate.notify_all();
		gate.wait_for(guard, std::chrono::seconds(2), [&]() { return gate_arrivals >= 2; });
	}
};

class SlowTransactionManager : public DuckTransactionManager {
public:
	SlowTransactionManager(AttachedDatabase &db, shared_ptr<ConcurrentStartState> state)
	    : DuckTransactionManager(db), state(std::move(state)) {
	}

public:
	Transaction &StartTransaction(ClientContext &context) override {
		auto attempt = ++state->starts;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (state->fail_first_start && attempt == 1) {
			throw IOException("first start fails");
		}
		return DuckTransactionManager::StartTransaction(context);
	}
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override {
		++state->commits;
		return DuckTransactionManager::CommitTransaction(context, transaction);
	}

private:
	shared_ptr<ConcurrentStartState> state;
};

struct SlowStorageExtension : StorageExtension {
	explicit SlowStorageExtension(shared_ptr<ConcurrentStartState> state) {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq<DuckCatalog>(db);
		};
		storage_info = make_shared_ptr<SlowInfo>(std::move(state));
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo> info, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<SlowTransactionManager>(db, dynamic_cast<SlowInfo &>(*info).state);
		};
	}

	struct SlowInfo : StorageExtensionInfo {
		explicit SlowInfo(shared_ptr<ConcurrentStartState> state) : state(std::move(state)) {
		}
		shared_ptr<ConcurrentStartState> state;
	};
};

struct TwoWorkersGlobalState : GlobalTableFunctionState {
	idx_t MaxThreads() const override {
		return 2;
	}
};

struct TwoWorkersInfo : TableFunctionInfo {
	explicit TwoWorkersInfo(shared_ptr<ConcurrentStartState> state) : state(std::move(state)) {
	}
	shared_ptr<ConcurrentStartState> state;
};

struct TwoWorkersBindData : TableFunctionData {
	explicit TwoWorkersBindData(shared_ptr<ConcurrentStartState> state) : state(std::move(state)) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<TwoWorkersBindData>(state);
	}
	bool Equals(const FunctionData &other) const override {
		return state == other.Cast<TwoWorkersBindData>().state;
	}
	shared_ptr<ConcurrentStartState> state;
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

TEST_CASE("Test two workers of a query that start the transaction of a database at the same time", "[api]") {
	auto state = make_shared_ptr<ConcurrentStartState>();
	DBConfig config;
	config.SetOptionByName("threads", Value::BIGINT(4));
	StorageExtension::Register(config, "slow_start", make_shared_ptr<SlowStorageExtension>(state));
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS slow_start (TYPE slow_start)"));

	// every worker returns the id of the transaction it got
	TableFunction two_workers(
	    "two_workers", {},
	    [](ClientContext &context, TableFunctionInput &input, DataChunk &output) {
		    auto &state = *input.bind_data->Cast<TwoWorkersBindData>().state;
		    if (state.next_slot++ >= 2) {
			    return;
		    }
		    state.WaitForOtherWorker();
		    auto target = DatabaseManager::Get(context).GetDatabase("slow_start");
		    optional_ptr<Transaction> transaction;
		    try {
			    transaction = Transaction::Get(context, *target);
		    } catch (std::exception &) {
			    if (!state.fail_first_start) {
				    throw;
			    }
			    transaction = Transaction::Get(context, *target);
		    }
		    auto writer = FlatVector::Writer<uint64_t>(output.data[0], 1);
		    writer.WriteValue(transaction->Cast<DuckTransaction>().GetTransactionId());
		    output.SetChildCardinality(1);
	    },
	    [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
	       vector<Identifier> &names) -> unique_ptr<FunctionData> {
		    return_types.push_back(LogicalType::UBIGINT);
		    names.push_back("transaction_id");
		    return make_uniq<TwoWorkersBindData>(input.info->Cast<TwoWorkersInfo>().state);
	    },
	    [](ClientContext &, TableFunctionInitInput &) -> unique_ptr<GlobalTableFunctionState> {
		    return make_uniq<TwoWorkersGlobalState>();
	    });
	two_workers.cardinality = [](ClientContext &, const FunctionData *) {
		return make_uniq<NodeStatistics>(1000000);
	};
	two_workers.function_info = make_shared_ptr<TwoWorkersInfo>(state);
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(two_workers);

	// a worker that waits for the start of the other gets the same transaction, also when it has to start it again
	for (auto fail_first : {false, true}) {
		state->Reset(fail_first);
		auto result = con.Query("SELECT count(*), min(transaction_id) = max(transaction_id) FROM two_workers()");
		REQUIRE(CHECK_COLUMN(result, 0, {2}));
		REQUIRE(CHECK_COLUMN(result, 1, {true}));
		REQUIRE(state->starts == (fail_first ? 2 : 1));
		REQUIRE(state->commits == 1);
	}
}
