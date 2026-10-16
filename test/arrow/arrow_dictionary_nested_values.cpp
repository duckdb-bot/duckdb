// Arrow scans of dictionary-encoded columns whose dictionary values are structs or fixed-size lists.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <deque>
#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_dictionary_nested_values_test {

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
	vector<int8_t> indices;
	vector<uint8_t> validity;
};

// Builds Arrow arrays, each node keeps its buffers
struct Builder {
	std::deque<Node> nodes;

	Node &NewNode(const char *format, const char *name, int64_t length, vector<Node *> children) {
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
		node.array.length = length;
		node.array.n_children = int64_t(children.size());
		node.array.children = node.array_children.data();
		node.array.release = ReleaseArray;
		return node;
	}
	void SetNulls(Node &node, const vector<idx_t> &null_slots) {
		node.validity.resize((idx_t(node.array.length) + 7) / 8, 0xFF);
		for (auto slot : null_slots) {
			node.validity[slot / 8] &= uint8_t(~(1u << (slot % 8)));
		}
		node.array.null_count = int64_t(null_slots.size());
		node.buffers[0] = null_slots.empty() ? nullptr : node.validity.data();
		node.schema.flags = ARROW_FLAG_NULLABLE;
	}
	Node &Int32(const char *name, vector<int32_t> values, const vector<idx_t> &null_slots = {}) {
		auto &node = NewNode("i", name, int64_t(values.size()), {});
		node.values = std::move(values);
		node.buffers = {nullptr, node.values.data()};
		node.array.n_buffers = 2;
		node.array.buffers = node.buffers.data();
		SetNulls(node, null_slots);
		return node;
	}
	Node &Int8(const char *name, vector<int8_t> values) {
		auto &node = NewNode("c", name, int64_t(values.size()), {});
		node.indices = std::move(values);
		node.buffers = {nullptr, node.indices.data()};
		node.array.n_buffers = 2;
		node.array.buffers = node.buffers.data();
		return node;
	}
	Node &Struct(const char *name, vector<Node *> children, const vector<idx_t> &null_slots = {}) {
		auto &node = NewNode("+s", name, children[0]->array.length, children);
		node.buffers = {nullptr};
		node.array.n_buffers = 1;
		node.array.buffers = node.buffers.data();
		SetNulls(node, null_slots);
		return node;
	}
	Node &FixedSizeList(const char *format, const char *name, int64_t length, Node &child) {
		auto &node = NewNode(format, name, length, {&child});
		node.buffers = {nullptr};
		node.array.n_buffers = 1;
		node.array.buffers = node.buffers.data();
		return node;
	}
	// int8 indices into the dictionary values, -1 is NULL
	Node &Dictionary(const char *name, vector<int8_t> indices, Node &values) {
		auto &node = NewNode("c", name, int64_t(indices.size()), {});
		node.validity.resize((indices.size() + 7) / 8, 0xFF);
		for (idx_t i = 0; i < indices.size(); i++) {
			if (indices[i] < 0) {
				node.validity[i / 8] &= uint8_t(~(1u << (i % 8)));
				node.array.null_count++;
				indices[i] = 0;
			}
		}
		node.indices = std::move(indices);
		node.buffers = {node.array.null_count ? node.validity.data() : nullptr, node.indices.data()};
		node.array.n_buffers = 2;
		node.array.buffers = node.buffers.data();
		node.schema.flags = ARROW_FLAG_NULLABLE;
		node.schema.dictionary = &values.schema;
		node.array.dictionary = &values.array;
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
	REQUIRE_NO_FAIL(*result);
	auto expected = con.Query(query);
	REQUIRE_NO_FAIL(*expected);
	return result->Equals(*expected, false);
}

} // namespace arrow_dictionary_nested_values_test

using arrow_dictionary_nested_values_test::Builder;
using arrow_dictionary_nested_values_test::ScanMatches;

TEST_CASE("Arrow scan of a dictionary whose values are a struct", "[arrow]") {
	Builder b;
	auto &values = b.Struct("", {&b.Int32("d", {30, 20, 10})});
	auto &column = b.Dictionary("s", {0, 2, -1, 1, 0}, values);
	REQUIRE(ScanMatches(column, "SELECT s::STRUCT(d INT) FROM (VALUES ({'d': 30}), ({'d': 10}), (NULL), ({'d': 20}), "
	                            "({'d': 30})) t(s)"));
}

TEST_CASE("Arrow scan of a dictionary whose values are a nested struct", "[arrow]") {
	Builder b;
	auto &inner = b.Struct("e", {&b.Int32("i", {1, 2, 3}), &b.Int32("j", {10, 20, 30})});
	auto &values = b.Struct("", {&inner});
	auto &column = b.Dictionary("s", {2, -1, 0}, values);
	REQUIRE(ScanMatches(column, "SELECT s::STRUCT(e STRUCT(i INT, j INT)) FROM (VALUES ({'e': {'i': 3, 'j': 30}}), "
	                            "(NULL), ({'e': {'i': 1, 'j': 10}})) t(s)"));
}

TEST_CASE("Arrow scan of a dictionary whose values are fixed-size lists", "[arrow]") {
	Builder b;
	auto &values = b.FixedSizeList("+w:2", "", 3, b.Int32("item", {1, 2, 3, 4, 5, 6}));
	auto &column = b.Dictionary("a", {1, -1, 2, 0}, values);
	REQUIRE(ScanMatches(column, "SELECT a::INT[2] FROM (VALUES ([3, 4]), (NULL), ([5, 6]), ([1, 2])) t(a)"));

	// a struct with a fixed-size list field
	Builder c;
	auto &list = c.FixedSizeList("+w:2", "l", 3, c.Int32("item", {1, 2, 3, 4, 5, 6}));
	auto &struct_values = c.Struct("", {&list});
	auto &struct_column = c.Dictionary("s", {2, 0, -1}, struct_values);
	REQUIRE(ScanMatches(struct_column,
	                    "SELECT s::STRUCT(l INT[2]) FROM (VALUES ({'l': [5, 6]}), ({'l': [1, 2]}), (NULL)) t(s)"));
}

TEST_CASE("Arrow scan of a dictionary of structs across several chunks", "[arrow]") {
	Builder b;
	auto &values = b.Struct("", {&b.Int32("d", {30, 20, 10})});
	vector<int8_t> indices;
	for (idx_t i = 0; i < 5000; i++) {
		indices.push_back(i % 7 == 0 ? int8_t(-1) : int8_t(i % 3));
	}
	auto &column = b.Dictionary("s", indices, values);
	REQUIRE(ScanMatches(column, "SELECT CASE WHEN i % 7 = 0 THEN NULL ELSE {'d': ([30, 20, 10])[i % 3 + 1]} "
	                            "END::STRUCT(d INT) FROM range(5000) t(i)"));
}

TEST_CASE("Arrow scan of a dictionary of structs with NULL entries and fields", "[arrow]") {
	Builder b;
	// entry 1 is a NULL struct, and the field of entry 2 is NULL
	auto &values = b.Struct("", {&b.Int32("d", {30, 20, 10}, {2})}, {1});
	auto &column = b.Dictionary("s", {0, 1, 2, -1, 0}, values);
	REQUIRE(ScanMatches(column, "SELECT s::STRUCT(d INT) FROM (VALUES ({'d': 30}), (NULL), ({'d': NULL}), (NULL), "
	                            "({'d': 30})) t(s)"));
}

TEST_CASE("Arrow scan of a dictionary of structs with a dictionary field", "[arrow]") {
	Builder b;
	// the field has int8 values, the type of its indices
	auto &field = b.Dictionary("f", {2, 0, 1}, b.Int8("", {7, 8, 9}));
	auto &values = b.Struct("", {&field});
	auto &column = b.Dictionary("s", {1, -1, 0, 2}, values);
	REQUIRE(ScanMatches(column, "SELECT s::STRUCT(f TINYINT) FROM (VALUES ({'f': 7}), (NULL), ({'f': 9}), ({'f': 8})) "
	                            "t(s)"));
}
