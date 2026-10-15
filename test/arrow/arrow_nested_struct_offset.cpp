// Arrow scans of sliced structs and sparse unions below a struct or a list, as produced by e.g. pyarrow slicing.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <deque>
#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_nested_struct_offset_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

struct Node {
	ArrowSchema schema {};
	ArrowArray array {};
	vector<const void *> buffers;
	vector<ArrowSchema *> schema_children;
	vector<ArrowArray *> array_children;
	vector<int32_t> values;
	vector<int8_t> type_ids;
};

// Builds Arrow arrays without NULLs, each node keeps its buffers
struct Builder {
	std::deque<Node> nodes;

	Node &NewNode(const char *format, const char *name, int64_t length, int64_t offset, vector<Node *> children) {
		nodes.emplace_back();
		auto &node = nodes.back();
		for (auto child : children) {
			node.schema_children.push_back(&child->schema);
			node.array_children.push_back(&child->array);
		}
		node.schema.format = format;
		node.schema.name = name;
		node.schema.n_children = int64_t(children.size());
		node.schema.children = node.schema_children.data();
		node.schema.release = ReleaseSchema;
		node.array.length = length - offset;
		node.array.offset = offset;
		node.array.n_children = int64_t(children.size());
		node.array.children = node.array_children.data();
		node.array.release = ReleaseArray;
		return node;
	}
	Node &Int32(const char *name, vector<int32_t> values) {
		auto &node = NewNode("i", name, int64_t(values.size()), 0, {});
		node.values = std::move(values);
		node.buffers = {nullptr, node.values.data()};
		node.array.n_buffers = 2;
		node.array.buffers = node.buffers.data();
		return node;
	}
	Node &Struct(const char *name, vector<Node *> children, int64_t offset = 0) {
		auto &node = NewNode("+s", name, children[0]->array.length, offset, children);
		node.buffers = {nullptr};
		node.array.n_buffers = 1;
		node.array.buffers = node.buffers.data();
		return node;
	}
	Node &Union(const char *name, vector<int8_t> type_ids, vector<Node *> children, int64_t offset = 0) {
		auto &node = NewNode("+us:0,1", name, int64_t(type_ids.size()), offset, children);
		node.type_ids = std::move(type_ids);
		node.buffers = {node.type_ids.data()};
		node.array.n_buffers = 1;
		node.array.buffers = node.buffers.data();
		return node;
	}
	Node &List(const char *name, vector<int32_t> offsets, Node &child) {
		auto &node = NewNode("+l", name, int64_t(offsets.size()) - 1, 0, {&child});
		node.values = std::move(offsets);
		node.buffers = {nullptr, node.values.data()};
		node.array.n_buffers = 2;
		node.array.buffers = node.buffers.data();
		return node;
	}
};

// Compares the rows of a scan of the column with those of the query, in order
bool ScanMatches(Node &column, const string &query) {
	Builder builder;
	auto &record = builder.Struct("", {&column});
	ArrowArrayStream stream {};
	AdbcError err {};
	REQUIRE(duckdb_adbc::BatchToArrayStream(&record.array, &record.schema, &stream, &err) == ADBC_STATUS_OK);
	DuckDB db(nullptr);
	Connection con(db);
	auto result = con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))->Execute();
	auto expected = con.Query(query);
	if (result->HasError()) {
		INFO(result->GetError());
		return false;
	}
	return result->Equals(*expected, false);
}

// A value of the union of the int32 members i and j
string Member(const string &name, int32_t value) {
	return "union_value(" + name + " := " + to_string(value) + ")::UNION(i INT, j INT)";
}

} // namespace arrow_nested_struct_offset_test

using arrow_nested_struct_offset_test::Builder;
using arrow_nested_struct_offset_test::Member;
using arrow_nested_struct_offset_test::ScanMatches;

// i and j hold 1 to 6 and 10 to 60, the sliced arrays below start at slot 2

TEST_CASE("Arrow scan of a sliced struct in a list", "[arrow]") {
	Builder b;
	auto &s = b.Struct("item", {&b.Int32("i", {1, 2, 3, 4, 5, 6}), &b.Int32("j", {10, 20, 30, 40, 50, 60})}, 2);
	auto &l = b.List("c", {0, 2, 4}, s);
	REQUIRE(ScanMatches(l, "SELECT * FROM (VALUES ([{'i': 3, 'j': 30}, {'i': 4, 'j': 40}]), ([{'i': 5, 'j': 50}, "
	                       "{'i': 6, 'j': 60}])) t(c)"));
}

TEST_CASE("Arrow scan of a sliced sparse union in a list", "[arrow]") {
	Builder b;
	auto &u = b.Union("item", {0, 1, 0, 1, 0, 1},
	                  {&b.Int32("i", {1, 2, 3, 4, 5, 6}), &b.Int32("j", {10, 20, 30, 40, 50, 60})}, 2);
	auto &l = b.List("c", {0, 2, 4}, u);
	REQUIRE(ScanMatches(l, "SELECT [" + Member("i", 3) + ", " + Member("j", 40) + "] UNION ALL SELECT [" +
	                           Member("i", 5) + ", " + Member("j", 60) + "]"));
}

TEST_CASE("Arrow scan of a struct in a sliced struct", "[arrow]") {
	Builder b;
	auto &inner = b.Struct("y", {&b.Int32("i", {1, 2, 3, 4, 5, 6}), &b.Int32("j", {10, 20, 30, 40, 50, 60})});
	auto &middle = b.Struct("x", {&inner});
	auto &outer = b.Struct("c", {&middle}, 2);
	REQUIRE(ScanMatches(outer, "SELECT {'x': {'y': {'i': i::INT, 'j': (i * 10)::INT}}} FROM range(3, 7) t(i)"));
}

TEST_CASE("Arrow scan of a list and a union in a sliced struct in a list", "[arrow]") {
	Builder b;
	auto &inner_list = b.List("l", {0, 1, 2, 3, 4, 5, 6}, b.Int32("item", {1, 2, 3, 4, 5, 6}));
	auto &u =
	    b.Union("u", {1, 0, 1, 0, 1, 0}, {&b.Int32("i", {1, 2, 3, 4, 5, 6}), &b.Int32("j", {10, 20, 30, 40, 50, 60})});
	auto &s = b.Struct("item", {&inner_list, &u}, 2);
	auto &l = b.List("c", {0, 2, 4}, s);
	REQUIRE(ScanMatches(l, "SELECT [{'l': [3], 'u': " + Member("j", 30) + "}, {'l': [4], 'u': " + Member("i", 4) +
	                           "}] UNION ALL SELECT [{'l': [5], 'u': " + Member("j", 50) +
	                           "}, {'l': [6], 'u': " + Member("i", 6) + "}]"));
}
