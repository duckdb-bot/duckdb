#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"

using namespace duckdb;

// Many short queries that each build and drop small hash tables, so most nodes of the eviction queues die
DUCKDB_BENCHMARK(ShortJoinQueries, "[micro]")
static constexpr idx_t JOIN_COUNT = 6;
static constexpr idx_t QUERY_COUNT = 2000;
string query;
void Load(DuckDBBenchmarkState *state) override {
	state->conn.Query("SET threads = 2");
	state->conn.Query(
	    "CREATE TABLE src AS SELECT i AS id, 'k' || (i % 500) AS jkey, 'v' || i AS c1 FROM range(1000) t(i)");
	query = "SELECT src.id, src.c1 FROM src";
	for (idx_t j = 0; j < JOIN_COUNT; j++) {
		auto table = "t" + to_string(j);
		state->conn.Query("CREATE TABLE " + table +
		                  " AS SELECT 'k' || (i % 500) AS jkey, 'x_' || i AS val FROM range(1000) t(i)");
		query += " LEFT JOIN " + table + " ON src.jkey = " + table + ".jkey";
	}
	query += " LIMIT 100";
}
void RunBenchmark(DuckDBBenchmarkState *state) override {
	for (idx_t i = 0; i < QUERY_COUNT; i++) {
		state->result = state->conn.Query(query);
	}
}
string VerifyResult(QueryResult *result) override {
	if (result->HasError()) {
		return result->GetError();
	}
	auto &materialized = result->Cast<MaterializedQueryResult>();
	if (materialized.RowCount() != 100) {
		return "expected 100 rows, got " + to_string(materialized.RowCount());
	}
	return string();
}
string BenchmarkInfo() override {
	return "Run a query with six LEFT JOINs of small tables 2000 times";
}
FINISH_BENCHMARK(ShortJoinQueries)
