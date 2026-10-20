#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"

using namespace duckdb;

// Plans one SELECT with 1600 three-arm CASE expressions
DUCKDB_BENCHMARK(WideCaseProjectionPlanning, "[optimizer]")
void Load(DuckDBBenchmarkState *state) override {
}
string GetQuery() override {
	string query = "EXPLAIN SELECT ";
	for (idx_t k = 1; k <= 1600; k++) {
		auto i = to_string(k);
		if (k > 1) {
			query += ", ";
		}
		query += "CASE WHEN i % " + i + " = 0 THEN 'aaaaaaaaaa" + i + "' WHEN i % " + i + " = 1 THEN 'bbbbbbbbbb" + i +
		         "' ELSE 'cccccccccc" + i + "' END AS col" + i;
	}
	return query + " FROM range(10) t(i)";
}
string VerifyResult(QueryResult *result) override {
	if (result->HasError()) {
		return result->GetError();
	}
	return string();
}
string BenchmarkInfo() override {
	return "Plan a SELECT of 1600 CASE expressions";
}
FINISH_BENCHMARK(WideCaseProjectionPlanning)
