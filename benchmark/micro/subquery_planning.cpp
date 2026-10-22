#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"

using namespace duckdb;

//! A UNION ALL of one subquery with CTEs and window functions per pair of columns, as statistics libraries emit
static string UnionAllOfPairSubqueries(idx_t column_count) {
	string query = "SELECT count(*) FROM (";
	idx_t branch = 0;
	for (idx_t i = 0; i < column_count; i++) {
		for (idx_t j = 0; j < column_count; j++) {
			if (i == j) {
				continue;
			}
			auto a = "c" + to_string(i);
			auto b = "c" + to_string(j);
			if (branch > 0) {
				query += " UNION ALL ";
			}
			query += "SELECT " + to_string(branch) + " AS pair_idx, value FROM (WITH src AS (SELECT " + a + ", " + b +
			         " FROM t), data AS (SELECT " + a + ", " + b + " FROM src WHERE " + a + " IS NOT NULL AND " + b +
			         " IS NOT NULL), ranks AS (SELECT row_number() OVER (ORDER BY " + a +
			         ") AS sort_order, row_number() OVER (ORDER BY " + b +
			         ") AS r, count(*) OVER () AS n FROM data), lag_cte AS (SELECT abs(r - lag(r) OVER (ORDER BY "
			         "sort_order)) AS d, n FROM ranks) SELECT 1.0 - 3.0 * sum(d) / (max(n)::DOUBLE * max(n) - 1) AS "
			         "value FROM lag_cte WHERE d IS NOT NULL) sub_" +
			         to_string(branch);
			branch++;
		}
	}
	return query + ")";
}

DUCKDB_BENCHMARK(UnionAllManySubqueries, "[subquery]")
void Load(DuckDBBenchmarkState *state) override {
	string create = "CREATE TABLE t AS SELECT ";
	for (idx_t i = 0; i < 40; i++) {
		create += (i > 0 ? ", " : "") + to_string(i) + "::DOUBLE AS c" + to_string(i);
	}
	state->conn.Query(create);
}
string GetQuery() override {
	return UnionAllOfPairSubqueries(40);
}
string VerifyResult(QueryResult *result) override {
	if (result->HasError()) {
		return result->GetError();
	}
	auto count = result->Collection().GetValue(0, 0);
	if (count != Value::BIGINT(1560)) {
		return "expected 1560 rows, got " + count.ToString();
	}
	return string();
}
string BenchmarkInfo() override {
	return "Plan and run a UNION ALL of 1560 subqueries with CTEs and window functions over a one-row table";
}
FINISH_BENCHMARK(UnionAllManySubqueries)
