#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

void SlowBindExec(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<int32_t, int32_t>(args.data[0], result, args.size(), [](int32_t v) { return v; });
}

// keeps the index bind going while the writer commits
unique_ptr<FunctionData> SlowBindBind(BindScalarFunctionInput &input) {
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	return nullptr;
}

void RegisterSlowBind(Connection &con) {
	ScalarFunction fn("slow_bind", {LogicalType::INTEGER}, LogicalType::INTEGER, SlowBindExec, SlowBindBind);
	CreateScalarFunctionInfo info(fn);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
}

} // namespace

// A commit that may checkpoint checks the unbound indexes, while binding an index expression looks up catalog entries
TEST_CASE("A commit that may checkpoint does not wait on a concurrent index bind", "[storage]") {
	auto path = TestCreatePath("wal_unbound_index_bind_race.db");
	DeleteDatabase(path);
	{
		DuckDB db(path);
		Connection con(db);
		RegisterSlowBind(con);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER, k VARCHAR)"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE other(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CREATE MACRO norm(s) AS lower(s)"));
		// the macro is looked up after slow_bind is bound
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX idx ON t((slow_bind(i) + length(norm(k))))"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (7, 'k7'), (8, 'k8')"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	for (idx_t attempt = 0; attempt < 3; attempt++) {
		DuckDB db(path);
		Connection lookup(db);
		Connection writer(db);
		RegisterSlowBind(lookup);
		REQUIRE_NO_FAIL(writer.Query("SET wal_autocheckpoint = '0B'"));

		// the writer commits, and so checks the unbound indexes, until the lookup has bound the index of t
		std::atomic<bool> lookup_done {false};
		string writer_error;
		std::thread writer_thread([&]() {
			while (!lookup_done) {
				auto result = writer.Query("INSERT INTO other VALUES (1)");
				if (result->HasError()) {
					writer_error = result->GetError();
					return;
				}
			}
		});
		auto lookup_result = lookup.Query("SELECT count(*) FROM t WHERE k = 'k7'");
		lookup_done = true;
		writer_thread.join();
		REQUIRE(writer_error.empty());
		REQUIRE(CHECK_COLUMN(lookup_result, 0, {1}));
	}
	DeleteDatabase(path);
}
