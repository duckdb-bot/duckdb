#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace duckdb;

namespace {

void ListViewReleaseSchema(ArrowSchema *schema) {
	schema->release = nullptr;
}

void ListViewReleaseArray(ArrowArray *array) {
	array->release = nullptr;
}

template <class OFFSET_TYPE>
struct ListViewColumn {
	ListViewColumn(std::vector<OFFSET_TYPE> offsets_p, std::vector<OFFSET_TYPE> sizes_p, std::vector<int32_t> values_p)
	    : offsets(std::move(offsets_p)), sizes(std::move(sizes_p)), values(std::move(values_p)) {
		child_schema.format = "i";
		child_schema.name = "item";
		child_schema.flags = 2;
		child_schema.release = ListViewReleaseSchema;
		child_buffers[0] = nullptr;
		child_buffers[1] = values.data();
		child_array.length = int64_t(values.size());
		child_array.n_buffers = 2;
		child_array.buffers = child_buffers;
		child_array.release = ListViewReleaseArray;

		schema.format = sizeof(OFFSET_TYPE) == sizeof(int32_t) ? "+vl" : "+vL";
		schema.name = "a";
		schema.flags = 2;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ListViewReleaseSchema;
		schema_children[0] = &child_schema;

		buffers[0] = nullptr;
		buffers[1] = offsets.data();
		buffers[2] = sizes.data();
		array.length = int64_t(offsets.size());
		array.n_buffers = 3;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ListViewReleaseArray;
		array_children[0] = &child_array;
	}
	ListViewColumn(const ListViewColumn &) = delete;

	std::vector<OFFSET_TYPE> offsets;
	std::vector<OFFSET_TYPE> sizes;
	std::vector<int32_t> values;
	const void *child_buffers[2];
	const void *buffers[3];
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	ArrowSchema child_schema {};
	ArrowSchema schema {};
	ArrowArray child_array {};
	ArrowArray array {};
};

bool ListViewScanMatches(ArrowSchema &column_schema, ArrowArray &column_array, const string &query) {
	ArrowSchema *schema_children[1] = {&column_schema};
	ArrowSchema record_schema {};
	record_schema.format = "+s";
	record_schema.n_children = 1;
	record_schema.children = schema_children;
	record_schema.release = ListViewReleaseSchema;

	ArrowArray *array_children[1] = {&column_array};
	const void *buffers[1] = {nullptr};
	ArrowArray record_array {};
	record_array.length = column_array.length;
	record_array.n_buffers = 1;
	record_array.buffers = buffers;
	record_array.n_children = 1;
	record_array.children = array_children;
	record_array.release = ListViewReleaseArray;

	ArrowArrayStream stream {};
	AdbcError error {};
	if (duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &error) != ADBC_STATUS_OK) {
		return false;
	}
	DuckDB db(nullptr);
	Connection con(db);
	return ArrowTestHelper::RunArrowComparison(con, query, stream);
}

//! A list view of structs with a single integer field x
struct ListViewStructColumn {
	ListViewStructColumn(std::vector<int32_t> offsets_p, std::vector<int32_t> sizes_p, std::vector<int32_t> values_p)
	    : offsets(std::move(offsets_p)), sizes(std::move(sizes_p)), values(std::move(values_p)) {
		field_schema.format = "i";
		field_schema.name = "x";
		field_schema.flags = 2;
		field_schema.release = ListViewReleaseSchema;
		field_buffers[0] = nullptr;
		field_buffers[1] = values.data();
		field_array.length = int64_t(values.size());
		field_array.n_buffers = 2;
		field_array.buffers = field_buffers;
		field_array.release = ListViewReleaseArray;

		struct_schema.format = "+s";
		struct_schema.name = "item";
		struct_schema.flags = 2;
		struct_schema.n_children = 1;
		struct_schema.children = struct_schema_children;
		struct_schema.release = ListViewReleaseSchema;
		struct_schema_children[0] = &field_schema;
		struct_buffers[0] = nullptr;
		struct_array.length = int64_t(values.size());
		struct_array.n_buffers = 1;
		struct_array.buffers = struct_buffers;
		struct_array.n_children = 1;
		struct_array.children = struct_array_children;
		struct_array.release = ListViewReleaseArray;
		struct_array_children[0] = &field_array;

		schema.format = "+vl";
		schema.name = "a";
		schema.flags = 2;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ListViewReleaseSchema;
		schema_children[0] = &struct_schema;
		buffers[0] = nullptr;
		buffers[1] = offsets.data();
		buffers[2] = sizes.data();
		array.length = int64_t(offsets.size());
		array.n_buffers = 3;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ListViewReleaseArray;
		array_children[0] = &struct_array;
	}
	ListViewStructColumn(const ListViewStructColumn &) = delete;

	std::vector<int32_t> offsets;
	std::vector<int32_t> sizes;
	std::vector<int32_t> values;
	const void *field_buffers[2];
	const void *struct_buffers[1];
	const void *buffers[3];
	ArrowSchema *struct_schema_children[1];
	ArrowArray *struct_array_children[1];
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	ArrowSchema field_schema {};
	ArrowSchema struct_schema {};
	ArrowSchema schema {};
	ArrowArray field_array {};
	ArrowArray struct_array {};
	ArrowArray array {};
};

//! A list view of strings
struct ListViewStringColumn {
	ListViewStringColumn(std::vector<int32_t> offsets_p, std::vector<int32_t> sizes_p,
	                     const std::vector<std::string> &values)
	    : offsets(std::move(offsets_p)), sizes(std::move(sizes_p)) {
		string_offsets.push_back(0);
		for (auto &value : values) {
			data += value;
			string_offsets.push_back(int32_t(data.size()));
		}
		child_schema.format = "u";
		child_schema.name = "item";
		child_schema.flags = 2;
		child_schema.release = ListViewReleaseSchema;
		child_buffers[0] = nullptr;
		child_buffers[1] = string_offsets.data();
		child_buffers[2] = data.data();
		child_array.length = int64_t(values.size());
		child_array.n_buffers = 3;
		child_array.buffers = child_buffers;
		child_array.release = ListViewReleaseArray;

		schema.format = "+vl";
		schema.name = "a";
		schema.flags = 2;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ListViewReleaseSchema;
		schema_children[0] = &child_schema;
		buffers[0] = nullptr;
		buffers[1] = offsets.data();
		buffers[2] = sizes.data();
		array.length = int64_t(offsets.size());
		array.n_buffers = 3;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ListViewReleaseArray;
		array_children[0] = &child_array;
	}
	ListViewStringColumn(const ListViewStringColumn &) = delete;

	std::vector<int32_t> offsets;
	std::vector<int32_t> sizes;
	std::vector<int32_t> string_offsets;
	std::string data;
	const void *child_buffers[3];
	const void *buffers[3];
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	ArrowSchema child_schema {};
	ArrowSchema schema {};
	ArrowArray child_array {};
	ArrowArray array {};
};

//! Scans the column through arrow_scan and returns the rows of the expression as strings
vector<string> ListViewExpressionValues(ArrowSchema &column_schema, ArrowArray &column_array,
                                        const string &expression) {
	ArrowSchema *schema_children[1] = {&column_schema};
	ArrowSchema record_schema {};
	record_schema.format = "+s";
	record_schema.n_children = 1;
	record_schema.children = schema_children;
	record_schema.release = ListViewReleaseSchema;

	ArrowArray *array_children[1] = {&column_array};
	const void *buffers[1] = {nullptr};
	ArrowArray record_array {};
	record_array.length = column_array.length;
	record_array.n_buffers = 1;
	record_array.buffers = buffers;
	record_array.n_children = 1;
	record_array.children = array_children;
	record_array.release = ListViewReleaseArray;

	ArrowArrayStream stream {};
	AdbcError error {};
	REQUIRE(duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &error) == ADBC_STATUS_OK);
	DuckDB db(nullptr);
	Connection con(db);
	auto relation = con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream));
	auto result = relation->Project(expression)->Execute();
	REQUIRE(!result->HasError());
	vector<string> values;
	for (auto &row : *result) {
		values.push_back(row.GetValue<string>(0));
	}
	return values;
}

} // namespace

TEST_CASE("Arrow scan of ListView with disjoint child ranges", "[arrow]") {
	ListViewColumn<int32_t> column({4, 0}, {3, 2}, {0, 1, 2, 3, 4, 5, 6});
	REQUIRE(
	    ListViewScanMatches(column.schema, column.array,
	                        "SELECT CASE WHEN r = 0 THEN [4, 5, 6]::INT[] ELSE [0, 1]::INT[] END FROM range(2) t(r)"));
}

TEST_CASE("Arrow scan of ListView with overlapping child ranges", "[arrow]") {
	ListViewColumn<int64_t> column({0, 0}, {3, 3}, {10, 20, 30});
	REQUIRE(ListViewScanMatches(column.schema, column.array, "SELECT [10, 20, 30]::INT[] FROM range(2)"));
}

TEST_CASE("to_json of ListView entries that share child values", "[arrow]") {
	{
		DuckDB db(nullptr);
		if (!db.ExtensionIsLoaded("json")) {
			return;
		}
	}
	// rows 0 and 2 share the value 3, rows 1 and 3 share the values 1 and 2 (duckdblabs/bot-issue-tracker#506)
	ListViewColumn<int32_t> column({3, 0, 2, 1}, {2, 3, 2, 3}, {0, 1, 2, 3, 4});
	auto values = ListViewExpressionValues(column.schema, column.array, "to_json(a)::VARCHAR");
	REQUIRE(values == vector<string> {"[3,4]", "[0,1,2]", "[2,3]", "[1,2,3]"});

	ListViewColumn<int32_t> nested_column({3, 0, 2, 1}, {2, 3, 2, 3}, {0, 1, 2, 3, 4});
	values = ListViewExpressionValues(nested_column.schema, nested_column.array, "to_json({'l': a})::VARCHAR");
	REQUIRE(values == vector<string> {"{\"l\":[3,4]}", "{\"l\":[0,1,2]}", "{\"l\":[2,3]}", "{\"l\":[1,2,3]}"});

	// structs are nested values, so the copies are deep
	ListViewStructColumn struct_column({3, 0, 2, 1, 3}, {2, 3, 2, 3, 2}, {0, 1, 2, 3, 4});
	values = ListViewExpressionValues(struct_column.schema, struct_column.array, "to_json(a)::VARCHAR");
	REQUIRE(values == vector<string> {"[{\"x\":3},{\"x\":4}]", "[{\"x\":0},{\"x\":1},{\"x\":2}]",
	                                  "[{\"x\":2},{\"x\":3}]", "[{\"x\":1},{\"x\":2},{\"x\":3}]",
	                                  "[{\"x\":3},{\"x\":4}]"});

	// the copies of shared strings point at the same string, long ones too
	string long_string(100000, 'x');
	ListViewStringColumn string_column({3, 0, 2, 1}, {2, 3, 2, 3}, {"a", "b", long_string, "d", "e"});
	values = ListViewExpressionValues(string_column.schema, string_column.array, "to_json(a)::VARCHAR");
	REQUIRE(values == vector<string> {"[\"d\",\"e\"]", "[\"a\",\"b\",\"" + long_string + "\"]",
	                                  "[\"" + long_string + "\",\"d\"]", "[\"b\",\"" + long_string + "\",\"d\"]"});
}
