// Arrow scans of sparse unions with a nonzero offset or custom type ids, including dictionary-encoded members.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_union_offset_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

const int32_t DICT_VALUES[3] = {10, 20, 30};

// Sparse UNION(d dict-encoded int32, i int32) with the given offset and format - slot j holds type id type_ids[j],
// the dictionary member holds DICT_VALUES[j % 3] and the plain member holds 100 + j
struct UnionColumn {
	UnionColumn(const std::vector<int8_t> &type_ids_p, int64_t offset, const char *format = "+us:0,1")
	    : type_ids(type_ids_p) {
		auto slots = type_ids.size();
		indices.resize(slots);
		plain_values.resize(slots);
		for (idx_t j = 0; j < slots; j++) {
			indices[j] = int32_t(j % 3);
			plain_values[j] = int32_t(100 + j);
		}
		dict_value_schema.format = "i";
		dict_value_schema.release = ReleaseSchema;
		dict_schema.format = "i";
		dict_schema.name = "d";
		dict_schema.flags = 2;
		dict_schema.dictionary = &dict_value_schema;
		dict_schema.release = ReleaseSchema;
		plain_schema.format = "i";
		plain_schema.name = "i";
		plain_schema.flags = 2;
		plain_schema.release = ReleaseSchema;
		schema_children[0] = &dict_schema;
		schema_children[1] = &plain_schema;
		schema.format = format;
		schema.name = "u";
		schema.n_children = 2;
		schema.children = schema_children;
		schema.release = ReleaseSchema;

		dict_value_buffers[0] = nullptr;
		dict_value_buffers[1] = DICT_VALUES;
		dict_value_array.length = 3;
		dict_value_array.n_buffers = 2;
		dict_value_array.buffers = dict_value_buffers;
		dict_value_array.release = ReleaseArray;
		dict_buffers[0] = nullptr;
		dict_buffers[1] = indices.data();
		dict_array.length = int64_t(slots);
		dict_array.n_buffers = 2;
		dict_array.buffers = dict_buffers;
		dict_array.dictionary = &dict_value_array;
		dict_array.release = ReleaseArray;
		plain_buffers[0] = nullptr;
		plain_buffers[1] = plain_values.data();
		plain_array.length = int64_t(slots);
		plain_array.n_buffers = 2;
		plain_array.buffers = plain_buffers;
		plain_array.release = ReleaseArray;
		array_children[0] = &dict_array;
		array_children[1] = &plain_array;
		buffers[0] = type_ids.data();
		array.length = int64_t(slots) - offset;
		array.offset = offset;
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 2;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	UnionColumn(const UnionColumn &) = delete;

	std::vector<int8_t> type_ids;
	std::vector<int32_t> indices;
	std::vector<int32_t> plain_values;
	const void *dict_value_buffers[2];
	const void *dict_buffers[2];
	const void *plain_buffers[2];
	const void *buffers[1];
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	ArrowSchema dict_value_schema {};
	ArrowSchema dict_schema {};
	ArrowSchema plain_schema {};
	ArrowSchema schema {};
	ArrowArray dict_value_array {};
	ArrowArray dict_array {};
	ArrowArray plain_array {};
	ArrowArray array {};
};

// Scans the column, the result holds the error when the scan fails
unique_ptr<QueryResult> ScanColumn(Connection &con, ArrowSchema &col_schema, ArrowArray &col_array) {
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
	REQUIRE(duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) == ADBC_STATUS_OK);
	return con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))->Execute();
}

// Compares the rows of the scan with those of the query, in order
bool ScanMatches(ArrowSchema &col_schema, ArrowArray &col_array, const string &query) {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = ScanColumn(con, col_schema, col_array);
	auto expected = con.Query(query);
	return !result->HasError() && result->Equals(*expected, false);
}

string ScanError(ArrowSchema &col_schema, ArrowArray &col_array) {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = ScanColumn(con, col_schema, col_array);
	return result->HasError() ? result->GetError() : string();
}

// The error of converting the schema to a DuckDB type, which a scan raises before it takes the stream
string SchemaError(ArrowSchema &col_schema) {
	DuckDB db(nullptr);
	Connection con(db);
	try {
		ArrowType::GetArrowLogicalType(*con.context, col_schema);
	} catch (std::exception &ex) {
		return ErrorData(ex).Message();
	}
	return string();
}

} // namespace arrow_union_offset_test

TEST_CASE("Arrow scan of a sparse union with a nonzero offset", "[arrow]") {
	// slots 0 and 1 are skipped by the offset
	arrow_union_offset_test::UnionColumn col({1, 1, 0, 1, 0, 0}, 2);
	REQUIRE(arrow_union_offset_test::ScanMatches(
	    col.schema, col.array,
	    "SELECT u FROM (VALUES (union_value(d := 30)::UNION(d INT, i INT)), (union_value(i := 103)::UNION(d INT, i "
	    "INT)), (union_value(d := 20)::UNION(d INT, i INT)), (union_value(d := 30)::UNION(d INT, i INT))) t(u)"));
}

TEST_CASE("Arrow scan of a sparse union with custom type ids", "[arrow]") {
	// d has type id 5 and i has type id 7
	arrow_union_offset_test::UnionColumn col({7, 5, 7, 5}, 0, "+us:5,7");
	REQUIRE(arrow_union_offset_test::ScanMatches(
	    col.schema, col.array,
	    "SELECT u FROM (VALUES (union_value(i := 100)::UNION(d INT, i INT)), (union_value(d := 20)::UNION(d INT, i "
	    "INT)), (union_value(i := 102)::UNION(d INT, i INT)), (union_value(d := 10)::UNION(d INT, i INT))) t(u)"));

	// type ids that are member indexes in another order, with an offset
	arrow_union_offset_test::UnionColumn swapped({1, 0, 0, 1}, 1, "+us:1,0");
	REQUIRE(arrow_union_offset_test::ScanMatches(
	    swapped.schema, swapped.array,
	    "SELECT u FROM (VALUES (union_value(i := 101)::UNION(d INT, i INT)), (union_value(i := 102)::UNION(d INT, i "
	    "INT)), (union_value(d := 10)::UNION(d INT, i INT))) t(u)"));

	// the largest type id, and type ids with leading zeros
	arrow_union_offset_test::UnionColumn largest({0, 127}, 0, "+us:127,0");
	REQUIRE(arrow_union_offset_test::ScanMatches(
	    largest.schema, largest.array,
	    "SELECT u FROM (VALUES (union_value(i := 100)::UNION(d INT, i INT)), (union_value(d := 20)::UNION(d INT, i "
	    "INT))) t(u)"));
	arrow_union_offset_test::UnionColumn padded({1, 0}, 0, "+us:00,01");
	REQUIRE(arrow_union_offset_test::ScanMatches(
	    padded.schema, padded.array,
	    "SELECT u FROM (VALUES (union_value(i := 100)::UNION(d INT, i INT)), (union_value(d := 20)::UNION(d INT, i "
	    "INT))) t(u)"));
}

TEST_CASE("Arrow scan of a sparse union with invalid type ids", "[arrow]") {
	arrow_union_offset_test::UnionColumn unknown_id({5, 6}, 0, "+us:5,7");
	REQUIRE(StringUtil::Contains(arrow_union_offset_test::ScanError(unknown_id.schema, unknown_id.array),
	                             "type id 6 does not belong to a union member"));
	arrow_union_offset_test::UnionColumn negative_id({5, -1}, 0, "+us:5,7");
	REQUIRE(StringUtil::Contains(arrow_union_offset_test::ScanError(negative_id.schema, negative_id.array),
	                             "type id -1 does not belong to a union member"));

	const char *invalid_formats[] = {"+us:5",   "+us:5,7,9", "+us:5,5",  "+us:-1,0",
	                                 "+us:5,x", "+us:5,128", "+us:5, 7", "+us:"};
	for (auto format : invalid_formats) {
		INFO(format);
		arrow_union_offset_test::UnionColumn col({5, 5}, 0, format);
		REQUIRE(
		    StringUtil::Contains(arrow_union_offset_test::SchemaError(col.schema), "Invalid Input Error: Arrow union"));
	}
}
