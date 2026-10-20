#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"

using namespace duckdb;

// 1000 schemas of 20 tables each, next to one table in the pinned schema
#define PINNED_SCHEMA_BODY(QUERY)                                                                                      \
	void Load(DuckDBBenchmarkState *state) override {                                                                  \
		state->conn.Query("CREATE TABLE mine (i INTEGER)");                                                            \
		for (idx_t i = 0; i < 1000; i++) {                                                                             \
			auto schema = "s" + to_string(i);                                                                          \
			state->conn.Query("CREATE SCHEMA " + schema);                                                              \
			for (idx_t j = 0; j < 20; j++) {                                                                           \
				state->conn.Query("CREATE TABLE " + schema + ".t" + to_string(j) + " (a INTEGER)");                    \
			}                                                                                                          \
		}                                                                                                              \
	}                                                                                                                  \
	string GetQuery() override {                                                                                       \
		return QUERY;                                                                                                  \
	}                                                                                                                  \
	string VerifyResult(QueryResult *result) override {                                                                \
		if (result->HasError()) {                                                                                      \
			return result->GetError();                                                                                 \
		}                                                                                                              \
		auto row_count = result->RowCount();                                                                           \
		if (row_count != 1) {                                                                                          \
			return "expected only the table of the pinned schema, got " + to_string(row_count) + " rows";              \
		}                                                                                                              \
		return string();                                                                                               \
	}                                                                                                                  \
	string BenchmarkInfo() override {                                                                                  \
		return "Run " + string(QUERY) + " next to 1000 schemas of 20 tables";                                          \
	}

DUCKDB_BENCHMARK(DuckDBTablesPinnedSchema, "[system_functions]")
PINNED_SCHEMA_BODY("SELECT table_name FROM duckdb_tables() WHERE database_name = 'memory' AND schema_name = 'main'")
FINISH_BENCHMARK(DuckDBTablesPinnedSchema)

DUCKDB_BENCHMARK(ShowTablesPinnedSchema, "[system_functions]")
PINNED_SCHEMA_BODY("SHOW TABLES FROM memory.main")
FINISH_BENCHMARK(ShowTablesPinnedSchema)
