#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

using namespace duckdb;

namespace {

//! Holds an armed bind of slow_bind until the writer has committed while it binds
struct BindGate {
	mutex lock;
	std::condition_variable cv;
	bool armed = false;
	bool bind_started = false;
	bool binding = false;
	idx_t commits_while_binding = 0;
	string wal_size_while_binding;
};

BindGate gate;

void SlowBindExec(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<int32_t, int32_t>(args.data[0], result, args.size(), [](int32_t v) { return v; });
}

unique_ptr<FunctionData> SlowBindBind(BindScalarFunctionInput &input) {
	{
		unique_lock<mutex> guard(gate.lock);
		if (!gate.armed) {
			return nullptr;
		}
		gate.armed = false;
		gate.bind_started = true;
		gate.binding = true;
		gate.cv.notify_all();
		gate.cv.wait_for(guard, std::chrono::seconds(10), [&]() { return gate.commits_while_binding >= 2; });
	}
	// the bind holds the index entry while it reads the database from another connection
	Connection con(*input.GetClientContext().db);
	auto result = con.Query("SELECT wal_size FROM pragma_database_size() WHERE database_name = current_database()");
	if (result->HasError()) {
		result->ThrowError();
	}
	lock_guard<mutex> guard(gate.lock);
	gate.wal_size_while_binding = result->Collection().GetValue(0, 0).ToString();
	gate.binding = false;
	return nullptr;
}

void RegisterSlowBind(Connection &con) {
	ScalarFunction fn("slow_bind", {LogicalType::INTEGER}, LogicalType::INTEGER, SlowBindExec, SlowBindBind);
	CreateScalarFunctionInfo info(fn);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
}

string KeyQuery(idx_t key) {
	return "SELECT count(*) FROM t WHERE k = 'k" + to_string(key) + "'";
}

} // namespace

TEST_CASE("A commit that may checkpoint does not wait on a concurrent index bind", "[storage]") {
	auto path = TestCreatePath("wal_unbound_index_bind_race.db");
	DeleteDatabase(path);
	vector<idx_t> present {1, 2};
	vector<idx_t> deleted {0};
	auto check_keys = [&](Connection &con) {
		for (auto key : present) {
			auto result = con.Query(KeyQuery(key));
			REQUIRE(CHECK_COLUMN(result, 0, {1}));
		}
		for (auto key : deleted) {
			auto result = con.Query(KeyQuery(key));
			REQUIRE(CHECK_COLUMN(result, 0, {0}));
		}
	};
	{
		DuckDB db(path);
		Connection con(db);
		RegisterSlowBind(con);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER, k VARCHAR)"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE other(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CREATE MACRO norm(s) AS lower(s)"));
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX idx_k ON t(k)"));
		// the macro is looked up after slow_bind is bound
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX idx_slow ON t((slow_bind(i) + length(norm(k))))"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (0, 'k0'), (1, 'k1')"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		// these stay in the WAL, so that the replay buffers them in the unbound indexes
		REQUIRE_NO_FAIL(con.Query("SET wal_autocheckpoint = '1TB'"));
		REQUIRE_NO_FAIL(con.Query("PRAGMA disable_checkpoint_on_shutdown"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (2, 'k2')"));
		REQUIRE_NO_FAIL(con.Query("DELETE FROM t WHERE i = 0"));
	}
	for (idx_t attempt = 0; attempt < 3; attempt++) {
		DuckDB db(path);
		Connection first(db);
		Connection second(db);
		Connection writer(db);
		RegisterSlowBind(first);
		REQUIRE_NO_FAIL(writer.Query("SET wal_autocheckpoint = '0B'"));
		{
			lock_guard<mutex> guard(gate.lock);
			gate.armed = true;
			gate.bind_started = false;
			gate.commits_while_binding = 0;
			gate.wal_size_while_binding = string();
		}

		// the writer commits, and so checks the indexes for buffered replays, until both lookups are done
		std::atomic<bool> lookups_done {false};
		string writer_error;
		std::thread writer_thread([&]() {
			while (!lookups_done) {
				auto result = writer.Query("INSERT INTO other VALUES (1)");
				if (result->HasError()) {
					writer_error = result->GetError();
					return;
				}
				lock_guard<mutex> guard(gate.lock);
				if (gate.binding) {
					gate.commits_while_binding++;
					gate.cv.notify_all();
				}
			}
		});
		auto lookup_key = present.back();
		duckdb::unique_ptr<QueryResult> first_result;
		std::thread first_thread([&]() { first_result = first.Query(KeyQuery(lookup_key)); });
		{
			unique_lock<mutex> guard(gate.lock);
			gate.cv.wait_for(guard, std::chrono::seconds(10), [&]() { return gate.bind_started; });
		}
		// the second lookup waits for the first one to bind the indexes
		duckdb::unique_ptr<QueryResult> second_result;
		std::thread second_thread([&]() { second_result = second.Query(KeyQuery(lookup_key)); });
		first_thread.join();
		second_thread.join();
		lookups_done = true;
		writer_thread.join();

		REQUIRE(writer_error.empty());
		REQUIRE(CHECK_COLUMN(first_result, 0, {1}));
		REQUIRE(CHECK_COLUMN(second_result, 0, {1}));
		REQUIRE(gate.commits_while_binding >= 2);
		REQUIRE(gate.wal_size_while_binding != "0 bytes");
		check_keys(writer);

		// buffer another insert and delete for the next restart
		REQUIRE_NO_FAIL(writer.Query("SET wal_autocheckpoint = '1TB'"));
		REQUIRE_NO_FAIL(writer.Query("PRAGMA disable_checkpoint_on_shutdown"));
		auto new_key = present.back() + 1;
		REQUIRE_NO_FAIL(
		    writer.Query("INSERT INTO t VALUES (" + to_string(new_key) + ", 'k" + to_string(new_key) + "')"));
		REQUIRE_NO_FAIL(writer.Query("DELETE FROM t WHERE i = " + to_string(present.front())));
		deleted.push_back(present.front());
		present.erase(present.begin());
		present.push_back(new_key);
	}
	{
		DuckDB db(path);
		Connection con(db);
		RegisterSlowBind(con);
		check_keys(con);
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	{
		DuckDB db(path);
		Connection con(db);
		RegisterSlowBind(con);
		check_keys(con);
	}
	DeleteDatabase(path);
}
