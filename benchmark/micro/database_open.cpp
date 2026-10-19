#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"

using namespace duckdb;

#define OPEN_DATABASE_COUNT 50

DUCKDB_BENCHMARK(OpenInMemoryDatabases, "[micro]")
void Load(DuckDBBenchmarkState *state) override {
}
void RunBenchmark(DuckDBBenchmarkState *state) override {
	vector<unique_ptr<DuckDB>> databases;
	vector<unique_ptr<Connection>> connections;
	for (idx_t i = 0; i < OPEN_DATABASE_COUNT; i++) {
		databases.push_back(make_uniq<DuckDB>(nullptr));
		connections.push_back(make_uniq<Connection>(*databases.back()));
		connections.back()->Query("CREATE TABLE t1 (i1 INTEGER)");
	}
}
string VerifyResult(QueryResult *result) override {
	return string();
}
string BenchmarkInfo() override {
	return "Open " + to_string(OPEN_DATABASE_COUNT) +
	       " in-memory databases with the default settings, each with a connection and a table, then close them";
}
FINISH_BENCHMARK(OpenInMemoryDatabases)
