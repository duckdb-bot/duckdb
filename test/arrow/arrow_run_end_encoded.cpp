// Arrow scans of run-end encoded columns, at the top level and as children of LIST.

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

const int32_t STRING_DICT_OFFSETS[3] = {0, 1, 2};
const char STRING_DICT_DATA[] = "xy";
const int32_t DICT_RUN_ENDS[2] = {1, 2};

// Run-end encoded column of int8 indices into the string dictionary ["x", "y"], NULL for the given runs.
struct RunEndDictionaryColumn {
	RunEndDictionaryColumn(const std::vector<int32_t> &run_ends_p, const std::vector<int8_t> &indices_p,
	                       const std::vector<idx_t> &null_runs, bool run_end_dictionary = false)
	    : run_ends(run_ends_p), indices(indices_p), validity((indices_p.size() + 7) / 8 + 2, 0xFF) {
		for (auto run : null_runs) {
			validity[run / 8] &= uint8_t(~(1u << (run % 8)));
		}
		auto run_count = int64_t(run_ends.size());

		run_ends_schema.format = "i";
		run_ends_schema.name = "run_ends";
		run_ends_schema.release = ReleaseSchema;
		dict_schema.format = "u";
		dict_schema.release = ReleaseSchema;
		values_schema.format = "c";
		values_schema.name = "values";
		values_schema.flags = 2; // ARROW_FLAG_NULLABLE
		values_schema.dictionary = &dict_schema;
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
		dict_buffers[0] = nullptr;
		dict_buffers[1] = STRING_DICT_OFFSETS;
		dict_buffers[2] = STRING_DICT_DATA;
		dict_array.length = 2;
		dict_array.n_buffers = 3;
		dict_array.buffers = dict_buffers;
		dict_array.release = ReleaseArray;
		if (run_end_dictionary) {
			// the same dictionary as two runs of one value each
			dict_values_schema = dict_schema;
			dict_values_schema.name = "values";
			dict_run_ends_schema.format = "i";
			dict_run_ends_schema.name = "run_ends";
			dict_run_ends_schema.release = ReleaseSchema;
			dict_schema_children[0] = &dict_run_ends_schema;
			dict_schema_children[1] = &dict_values_schema;
			dict_schema.format = "+r";
			dict_schema.n_children = 2;
			dict_schema.children = dict_schema_children;
			dict_values_array = dict_array;
			dict_run_ends_buffers[0] = nullptr;
			dict_run_ends_buffers[1] = DICT_RUN_ENDS;
			dict_run_ends_array.length = 2;
			dict_run_ends_array.n_buffers = 2;
			dict_run_ends_array.buffers = dict_run_ends_buffers;
			dict_run_ends_array.release = ReleaseArray;
			dict_array_children[0] = &dict_run_ends_array;
			dict_array_children[1] = &dict_values_array;
			dict_array.n_buffers = 1;
			dict_array.buffers = dict_ree_buffers;
			dict_array.n_children = 2;
			dict_array.children = dict_array_children;
		}
		values_buffers[0] = validity.data();
		values_buffers[1] = indices.data();
		values_array.length = run_count;
		values_array.null_count = int64_t(null_runs.size());
		values_array.n_buffers = 2;
		values_array.buffers = values_buffers;
		values_array.dictionary = &dict_array;
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
	RunEndDictionaryColumn(const RunEndDictionaryColumn &) = delete;

	std::vector<int32_t> run_ends;
	std::vector<int8_t> indices;
	std::vector<uint8_t> validity;
	const void *run_ends_buffers[2];
	const void *dict_buffers[3];
	const void *dict_run_ends_buffers[2];
	const void *dict_ree_buffers[1] = {nullptr};
	ArrowSchema *dict_schema_children[2];
	ArrowArray *dict_array_children[2];
	ArrowSchema dict_run_ends_schema {};
	ArrowSchema dict_values_schema {};
	ArrowArray dict_run_ends_array {};
	ArrowArray dict_values_array {};
	const void *values_buffers[2];
	const void *buffers[1] = {nullptr};
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	ArrowSchema run_ends_schema {};
	ArrowSchema dict_schema {};
	ArrowSchema values_schema {};
	ArrowSchema schema {};
	ArrowArray run_ends_array {};
	ArrowArray dict_array {};
	ArrowArray values_array {};
	ArrowArray array {};
};

// LIST column over a run-end encoded child with the given offsets.
struct ListColumn {
	ListColumn(RunEndColumn &child, const std::vector<int32_t> &offsets_p) : offsets(offsets_p) {
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

// Compares the scan of the column with the query, in order and with the types when ordered is set.
bool ScanMatches(ArrowSchema &col_schema, ArrowArray &col_array, const string &query, bool ordered = false) {
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
	DuckDB db(nullptr);
	Connection con(db);
	if (!ordered) {
		return ArrowTestHelper::RunArrowComparison(con, query, stream);
	}
	auto result = con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))->Execute();
	stream.release = nullptr;
	auto expected = con.Query(query);
	return result->Equals(*expected, false);
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

TEST_CASE("Arrow scan of a run-end encoded dictionary column with NULL runs across chunks", "[arrow]") {
	arrow_run_end_encoded_test::RunEndDictionaryColumn col({1000, 3000, 5000}, {1, 0, 0}, {1});
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array,
	    "SELECT CASE WHEN r < 1000 THEN 'y' WHEN r < 3000 THEN NULL ELSE 'x' END FROM range(5000) t(r)", true));
}

TEST_CASE("Arrow scan of a run-end encoded dictionary column whose dictionary is run-end encoded", "[arrow]") {
	arrow_run_end_encoded_test::RunEndDictionaryColumn col({2, 4, 6}, {0, 1, 0}, {}, true);
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array, "SELECT * FROM (VALUES ('x'), ('x'), ('y'), ('y'), ('x'), ('x'))", true));
}
