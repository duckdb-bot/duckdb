#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"

using namespace duckdb;

// 1000 schemas of 20 tables each, next to one table and one macro in the pinned schema
#define PINNED_SCHEMA_BODY(QUERY)                                                                                      \
	void Load(DuckDBBenchmarkState *state) override {                                                                  \
		state->conn.Query("CREATE TABLE mine (i INTEGER)");                                                            \
		state->conn.Query("CREATE MACRO mine() AS 1");                                                                 \
		for (idx_t schema_idx = 0; schema_idx < 1000; schema_idx++) {                                                  \
			auto schema = "s" + to_string(schema_idx);                                                                 \
			state->conn.Query("CREATE SCHEMA " + schema);                                                              \
			for (idx_t table_idx = 0; table_idx < 20; table_idx++) {                                                   \
				state->conn.Query("CREATE TABLE " + schema + ".t" + to_string(table_idx) + " (a INTEGER)");            \
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
			return "expected only the entry of the pinned schema, got " + to_string(row_count) + " rows";              \
		}                                                                                                              \
		auto name = result->Collection().GetValue(0, 0);                                                               \
		if (name != Value("mine")) {                                                                                   \
			return "expected the entry mine, got " + name.ToString();                                                  \
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

DUCKDB_BENCHMARK(DuckDBFunctionsPinnedSchema, "[system_functions]")
PINNED_SCHEMA_BODY(
    "SELECT function_name FROM duckdb_functions() WHERE database_name = 'memory' AND schema_name = 'main'")
FINISH_BENCHMARK(DuckDBFunctionsPinnedSchema)
