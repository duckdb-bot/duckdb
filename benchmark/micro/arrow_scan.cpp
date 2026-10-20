#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"
#include "duckdb.h"
#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"

using namespace duckdb;

namespace {

static constexpr idx_t ARROW_SCAN_ROW_COUNT = 2000000;

//! One Arrow array of the values below 2,000,000 in a scrambled order
struct ArrowArrayScanState : public DuckDBBenchmarkState {
	ArrowArrayScanState() : DuckDBBenchmarkState(string()) {
		array.release = nullptr;
		schema.release = nullptr;
	}
	~ArrowArrayScanState() override {
		if (array.release) {
			array.release(&array);
		}
		if (schema.release) {
			schema.release(&schema);
		}
	}

	ArrowArray array;
	ArrowSchema schema;
};

} // namespace

DUCKDB_BENCHMARK(ArrowArrayScanTopN, "[arrow]")
duckdb::unique_ptr<DuckDBBenchmarkState> CreateBenchmarkState() override {
	return make_uniq<ArrowArrayScanState>();
}
void Load(DuckDBBenchmarkState *state_p) override {
	auto &state = static_cast<ArrowArrayScanState &>(*state_p);
	auto source = state.conn.Query("SELECT (i * 7919) % " + to_string(ARROW_SCAN_ROW_COUNT) + " AS price FROM range(" +
	                               to_string(ARROW_SCAN_ROW_COUNT) + ") t(i)");
	if (source->HasError()) {
		throw InternalException(source->GetError());
	}
	auto options = state.conn.context->GetClientProperties();
	unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> extension_type_cast;
	ArrowAppender appender(source->GetTypes(), ARROW_SCAN_ROW_COUNT, options, extension_type_cast);
	while (auto chunk = source->Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		appender.Append(*chunk, 0, chunk->size(), chunk->size());
	}
	state.array = appender.Finalize();
	ArrowConverter::ToArrowSchema(&state.schema, source->GetTypes(), IdentifiersToStrings(source->GetNames()), options);
}
void RunBenchmark(DuckDBBenchmarkState *state_p) override {
	auto &state = static_cast<ArrowArrayScanState &>(*state_p);
	// the view returns the array once, so each run scans it through a new view
	auto connection = reinterpret_cast<duckdb_connection>(&state.conn);
	duckdb_arrow_stream stream = nullptr;
	if (duckdb_arrow_array_scan(connection, "prices", reinterpret_cast<duckdb_arrow_schema>(&state.schema),
	                            reinterpret_cast<duckdb_arrow_array>(&state.array), &stream) != DuckDBSuccess) {
		throw InternalException("duckdb_arrow_array_scan failed");
	}
	state.result = state.conn.Query("SELECT price FROM prices ORDER BY price DESC LIMIT 10000");
	duckdb_destroy_arrow_stream(&stream);
}
string VerifyResult(QueryResult *result) override {
	if (result->HasError()) {
		return result->GetError();
	}
	if (result->RowCount() != 10000) {
		return "expected 10000 rows, got " + to_string(result->RowCount());
	}
	auto &rows = result->Collection();
	if (rows.GetValue(0, 0) != Value::BIGINT(1999999) || rows.GetValue(0, 9999) != Value::BIGINT(1990000)) {
		return "expected the 10000 largest prices in descending order";
	}
	return string();
}
string BenchmarkInfo() override {
	return "Scan one Arrow array of 2,000,000 rows with duckdb_arrow_array_scan and keep its 10,000 largest values";
}
FINISH_BENCHMARK(ArrowArrayScanTopN)
