// Arrow scans of run-end encoded columns, at the top level and as children of LIST and FIXED_SIZE_LIST.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_run_end_encoded_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

// Run-end encoded int32 column: run i ends at run_ends[i] and holds values[i], NULL for the given runs.
struct RunEndColumn {
	RunEndColumn(const std::vector<int32_t> &run_ends_p, const std::vector<int32_t> &values_p,
	             const std::vector<idx_t> &null_runs)
	    : run_ends(run_ends_p), values(values_p), validity((values_p.size() + 7) / 8 + 2, 0xFF) {
		for (auto run : null_runs) {
			validity[run / 8] &= uint8_t(~(1u << (run % 8)));
		}
		auto run_count = int64_t(run_ends.size());

		run_ends_schema.format = "i";
		run_ends_schema.name = "run_ends";
		run_ends_schema.release = ReleaseSchema;
		values_schema.format = "i";
		values_schema.name = "values";
		values_schema.flags = 2; // ARROW_FLAG_NULLABLE
		values_schema.release = ReleaseSchema;
		schema_children[0] = &run_ends_schema;
		schema_children[1] = &values_schema;
		schema.format = "+r";
		schema.name = "item";
		schema.flags = 2;
		schema.n_children = 2;
		schema.children = schema_children;
		schema.release = ReleaseSchema;

		run_ends_buffers[0] = nullptr;
		run_ends_buffers[1] = run_ends.data();
		run_ends_array.length = run_count;
		run_ends_array.n_buffers = 2;
		run_ends_array.buffers = run_ends_buffers;
		run_ends_array.release = ReleaseArray;
		values_buffers[0] = validity.data();
		values_buffers[1] = values.data();
		values_array.length = run_count;
		values_array.null_count = int64_t(null_runs.size());
		values_array.n_buffers = 2;
		values_array.buffers = values_buffers;
		values_array.release = ReleaseArray;
		array_children[0] = &run_ends_array;
		array_children[1] = &values_array;
		array.length = run_ends.empty() ? 0 : run_ends.back();
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 2;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	RunEndColumn(const RunEndColumn &) = delete;

	std::vector<int32_t> run_ends;
	std::vector<int32_t> values;
	std::vector<uint8_t> validity;
	const void *run_ends_buffers[2];
	const void *values_buffers[2];
	const void *buffers[1] = {nullptr};
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	ArrowSchema run_ends_schema {};
	ArrowSchema values_schema {};
	ArrowSchema schema {};
	ArrowArray run_ends_array {};
	ArrowArray values_array {};
	ArrowArray array {};
};

// LIST column over a run-end encoded or fixed-size list child with the given offsets.
struct ListColumn {
	template <class CHILD>
	ListColumn(CHILD &child, const std::vector<int32_t> &offsets_p) : offsets(offsets_p) {
		schema_children[0] = &child.schema;
		schema.format = "+l";
		schema.name = "a";
		schema.flags = 2;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = nullptr;
		buffers[1] = offsets.data();
		array_children[0] = &child.array;
		array.length = int64_t(offsets.size() - 1);
		array.n_buffers = 2;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	ListColumn(const ListColumn &) = delete;

	std::vector<int32_t> offsets;
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	const void *buffers[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

// FIXED_SIZE_LIST column of list_size elements over a run-end encoded child, starting at row offset, NULL for the
// given rows.
struct FixedSizeListColumn {
	FixedSizeListColumn(RunEndColumn &child, idx_t list_size, int64_t length, int64_t offset,
	                    const std::vector<idx_t> &null_rows = {})
	    : format("+w:" + std::to_string(list_size)), validity((length + offset + 7) / 8 + 1, 0xFF) {
		for (auto row : null_rows) {
			auto bit = row + idx_t(offset);
			validity[bit / 8] &= uint8_t(~(1u << (bit % 8)));
		}
		buffers[0] = null_rows.empty() ? nullptr : validity.data();
		schema_children[0] = &child.schema;
		schema.format = format.c_str();
		schema.name = "a";
		schema.flags = 2;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		array_children[0] = &child.array;
		array.length = length;
		array.offset = offset;
		array.null_count = int64_t(null_rows.size());
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	FixedSizeListColumn(const FixedSizeListColumn &) = delete;

	std::string format;
	std::vector<uint8_t> validity;
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	const void *buffers[1];
	ArrowSchema schema {};
	ArrowArray array {};
};

// Calls scan with a stream of one record batch that holds the column.
template <class SCAN>
bool WithStream(ArrowSchema &col_schema, ArrowArray &col_array, SCAN scan) {
	ArrowSchema *schema_children[1] = {&col_schema};
	ArrowSchema record_schema {};
	record_schema.format = "+s";
	record_schema.n_children = 1;
	record_schema.children = schema_children;
	record_schema.release = ReleaseSchema;

	ArrowArray *array_children[1] = {&col_array};
	const void *buffers[1] = {nullptr};
	ArrowArray record_array {};
	record_array.length = col_array.length;
	record_array.n_buffers = 1;
	record_array.buffers = buffers;
	record_array.n_children = 1;
	record_array.children = array_children;
	record_array.release = ReleaseArray;

	ArrowArrayStream stream {};
	AdbcError err {};
	if (duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) != ADBC_STATUS_OK) {
		return false;
	}
	return scan(stream);
}

bool ScanMatches(ArrowSchema &col_schema, ArrowArray &col_array, const string &query) {
	return WithStream(col_schema, col_array, [&](ArrowArrayStream &stream) {
		DuckDB db(nullptr);
		Connection con(db);
		return ArrowTestHelper::RunArrowComparison(con, query, stream);
	});
}

// Runs the query over the scan of the column, as the view arrow_view, and checks that it returns expected.
bool QueryMatches(ArrowSchema &col_schema, ArrowArray &col_array, const string &query, const string &expected) {
	return WithStream(col_schema, col_array, [&](ArrowArrayStream &stream) {
		DuckDB db(nullptr);
		Connection con(db);
		con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))->CreateView("arrow_view");
		auto result = con.Query(query);
		auto expected_result = con.Query(expected);
		stream.release = nullptr;
		if (result->HasError() || expected_result->HasError()) {
			return false;
		}
		return result->Equals(*expected_result, false);
	});
}

} // namespace arrow_run_end_encoded_test

TEST_CASE("Arrow scan of a run-end encoded column with NULL runs across chunks", "[arrow]") {
	arrow_run_end_encoded_test::RunEndColumn col({1000, 3000, 5000}, {1, 2, 3}, {1});
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array,
	    "SELECT CASE WHEN r < 1000 THEN 1 WHEN r < 3000 THEN NULL ELSE 3 END::INT FROM range(5000) t(r)"));
}

TEST_CASE("Arrow scan of LIST(run-end encoded) with a nonzero leading child offset", "[arrow]") {
	// child: [1, 1, 1, NULL, NULL, 3, 3, 3], the lists start at child slot 2
	arrow_run_end_encoded_test::RunEndColumn child({3, 5, 8}, {1, 2, 3}, {1});
	arrow_run_end_encoded_test::ListColumn col(child, {2, 4, 6, 8});
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array, "SELECT * FROM (VALUES ([1, NULL]::INT[]), ([NULL, 3]), ([3, 3]))"));
}

TEST_CASE("Arrow scan of LIST(run-end encoded) with leading empty lists", "[arrow]") {
	// child: [7, 7, NULL, 9]
	arrow_run_end_encoded_test::RunEndColumn child({2, 3, 4}, {7, 8, 9}, {1});
	arrow_run_end_encoded_test::ListColumn col(child, {0, 0, 0, 2, 4});
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(col.schema, col.array,
	                                                "SELECT * FROM (VALUES ([]::INT[]), ([]), ([7, 7]), ([NULL, 9]))"));
}

TEST_CASE("Arrow scan of LIST(run-end encoded) across chunks with a leading child offset", "[arrow]") {
	// child slot s holds s / 1000 + 1, NULL for slots [1000, 2000); row r covers child slots 3 + 2r and 4 + 2r
	arrow_run_end_encoded_test::RunEndColumn child({1000, 2000, 3000, 4000, 5000, 6003}, {1, 2, 3, 4, 5, 6}, {1});
	std::vector<int32_t> offsets;
	for (int32_t r = 0; r <= 3000; r++) {
		offsets.push_back(3 + 2 * r);
	}
	arrow_run_end_encoded_test::ListColumn col(child, offsets);
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array,
	    "SELECT [CASE WHEN s // 1000 = 1 THEN NULL ELSE LEAST(s // 1000, 5) + 1 END::INT FOR s IN [3 + 2 * r, 4 + 2 * "
	    "r]] FROM range(3000) t(r)"));
}

TEST_CASE("Arrow scan of FIXED_SIZE_LIST(run-end encoded)", "[arrow]") {
	// the child holds 10, 10, 10 and 20
	arrow_run_end_encoded_test::RunEndColumn child({3, 4}, {10, 20}, {});
	arrow_run_end_encoded_test::FixedSizeListColumn col(child, 2, 2, 0);
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(col.schema, col.array,
	                                                "SELECT * FROM (VALUES ([10, 10]::INT[2]), ([10, 20]))"));
}

TEST_CASE("Arrow scan of FIXED_SIZE_LIST(run-end encoded) across chunks with an offset", "[arrow]") {
	// child slot s holds s / 1000 + 1, and NULL for slots 1000 to 1999, and row r covers child slots 2 + 2r and 3 + 2r
	arrow_run_end_encoded_test::RunEndColumn child({1000, 2000, 3000, 4000, 5000, 6002}, {1, 2, 3, 4, 5, 6}, {1});
	arrow_run_end_encoded_test::FixedSizeListColumn col(child, 2, 3000, 1);
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array,
	    "SELECT [CASE WHEN s // 1000 = 1 THEN NULL ELSE LEAST(s // 1000, 5) + 1 END::INT FOR s IN [2 + 2 * r, 3 + 2 * "
	    "r]]::INT[2] FROM range(3000) t(r)"));
}

TEST_CASE("Arrow scan of FIXED_SIZE_LIST(run-end encoded) with NULL rows over values", "[arrow]") {
	// the child holds 1000, 1000, NULL, NULL, 5 and 5, and the first row, over the two values 1000, is NULL
	arrow_run_end_encoded_test::RunEndColumn child({2, 4, 6}, {1000, 0, 5}, {1});
	arrow_run_end_encoded_test::FixedSizeListColumn col(child, 2, 3, 0, {0});
	REQUIRE(arrow_run_end_encoded_test::QueryMatches(
	    col.schema, col.array, "SELECT a::TINYINT[2] FROM arrow_view",
	    "SELECT * FROM (VALUES (NULL::TINYINT[2]), ([NULL, NULL]::TINYINT[2]), ([5, 5]::TINYINT[2]))"));
}

TEST_CASE("Arrow scan of LIST(run-end encoded) with only empty lists", "[arrow]") {
	arrow_run_end_encoded_test::RunEndColumn child({2}, {7}, {});
	arrow_run_end_encoded_test::ListColumn col(child, {0, 0, 0});
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(col.schema, col.array, "SELECT * FROM (VALUES ([]::INT[]), ([]))"));
}

TEST_CASE("Arrow scan of LIST(FIXED_SIZE_LIST(run-end encoded)) with only empty lists", "[arrow]") {
	arrow_run_end_encoded_test::RunEndColumn child({2}, {7}, {});
	arrow_run_end_encoded_test::FixedSizeListColumn fixed_size_list(child, 2, 1, 0);
	arrow_run_end_encoded_test::ListColumn col(fixed_size_list, {0, 0, 0});
	REQUIRE(
	    arrow_run_end_encoded_test::ScanMatches(col.schema, col.array, "SELECT * FROM (VALUES ([]::INT[2][]), ([]))"));
}
