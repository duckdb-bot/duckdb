#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"
#include "duckdb/main/materialized_query_result.hpp"

using namespace duckdb;

#define LIST_LITERAL_COUNT 1000000

DUCKDB_BENCHMARK(LargeListLiteral, "[list]")
string query;
void Load(DuckDBBenchmarkState *state) override {
	query = "SELECT length([";
	for (idx_t i = 0; i < LIST_LITERAL_COUNT; i++) {
		if (i > 0) {
			query += ",";
		}
		auto tenths = (i % 977) * 15;
		query += to_string(tenths / 10) + "." + to_string(tenths % 10);
	}
	query += "]) AS n";
}
string GetQuery() override {
	return query;
}
string VerifyResult(QueryResult *result) override {
	if (result->HasError()) {
		return result->GetError();
	}
	auto &materialized = result->Cast<MaterializedQueryResult>();
	if (materialized.GetValue(0, 0) != Value::BIGINT(LIST_LITERAL_COUNT)) {
		return "Incorrect list length";
	}
	return string();
}
string BenchmarkInfo() override {
	return "Parse, bind and run a statement with a list literal of a million decimal constants";
}
FINISH_BENCHMARK(LargeListLiteral)
