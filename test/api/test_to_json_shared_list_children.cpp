#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

using namespace duckdb;

// Returns the children of each list between an offset and a length without copying them, so that the rows of a
// constant list share its children, as Arrow list views can
static void ListChildRange(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &list = args.data[0];
	auto lists = list.Values<list_entry_t>();
	auto offsets = args.data[1].Values<int64_t>();
	auto lengths = args.data[2].Values<int64_t>();
	auto entries = FlatVector::Writer<list_entry_t>(result, args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		if (!offsets[i].IsValid() || !lengths[i].IsValid()) {
			entries.WriteNull();
			continue;
		}
		const auto offset = lists[i].GetValue().offset + UnsafeNumericCast<idx_t>(offsets[i].GetValue());
		entries.WriteValue(list_entry_t(offset, UnsafeNumericCast<idx_t>(lengths[i].GetValue())));
	}
	ListVector::Append(result, ListVector::GetChild(list), ListVector::GetListSize(list));
}

static void RegisterListChildRange(Connection &con) {
	ScalarFunctionSet functions("list_child_range");
	auto list_type = LogicalType::LIST(LogicalType::TEMPLATE("T"));
	functions.AddFunction(
	    ScalarFunction({list_type, LogicalType::BIGINT, LogicalType::BIGINT}, list_type, ListChildRange));
	auto map_type = LogicalType::MAP(LogicalType::TEMPLATE("K"), LogicalType::TEMPLATE("V"));
	functions.AddFunction(
	    ScalarFunction({map_type, LogicalType::BIGINT, LogicalType::BIGINT}, map_type, ListChildRange));
	CreateScalarFunctionInfo info(functions);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
}

TEST_CASE("to_json of list entries that share children", "[api][json]") {
	DuckDB db(nullptr);
	if (!db.ExtensionIsLoaded("json")) {
		return;
	}
	Connection con(db);
	RegisterListChildRange(con);
	const string bounds = " FROM (VALUES (3, 2), (0, 3), (NULL, NULL), (0, 3), (2, 2), (1, 3), (3, 2)) t(o, n)";

	// rows 0 and 4 share the child 3 and rows 1 and 5 the children 1 and 2, while rows 3 and 6 repeat rows 1 and 0
	auto result = con.Query("SELECT to_json(list_child_range([0, 1, 2, 3, 4], o, n))" + bounds);
	REQUIRE(CHECK_COLUMN(result, 0, {"[3,4]", "[0,1,2]", Value(), "[0,1,2]", "[2,3]", "[1,2,3]", "[3,4]"}));

	result = con.Query("SELECT to_json(list_child_range(['a', 'b', 'c', 'd', 'e'], o, n))" + bounds);
	REQUIRE(CHECK_COLUMN(result, 0,
	                     {"[\"d\",\"e\"]", "[\"a\",\"b\",\"c\"]", Value(), "[\"a\",\"b\",\"c\"]", "[\"c\",\"d\"]",
	                      "[\"b\",\"c\",\"d\"]", "[\"d\",\"e\"]"}));

	result = con.Query("SELECT to_json(list_child_range([{'x': 0}, {'x': 1}, {'x': 2}, {'x': 3}, {'x': 4}], o, n))" +
	                   bounds);
	REQUIRE(CHECK_COLUMN(result, 0,
	                     {"[{\"x\":3},{\"x\":4}]", "[{\"x\":0},{\"x\":1},{\"x\":2}]", Value(),
	                      "[{\"x\":0},{\"x\":1},{\"x\":2}]", "[{\"x\":2},{\"x\":3}]", "[{\"x\":1},{\"x\":2},{\"x\":3}]",
	                      "[{\"x\":3},{\"x\":4}]"}));

	result = con.Query("SELECT to_json(list_child_range([[0], [1, 1], [], [3], [4, 4]], o, n))" + bounds);
	REQUIRE(CHECK_COLUMN(
	    result, 0,
	    {"[[3],[4,4]]", "[[0],[1,1],[]]", Value(), "[[0],[1,1],[]]", "[[],[3]]", "[[1,1],[],[3]]", "[[3],[4,4]]"}));

	result = con.Query("SELECT to_json(list_child_range(MAP {'a': 0, 'b': 1, 'c': 2, 'd': 3, 'e': 4}, o, n))" + bounds);
	REQUIRE(CHECK_COLUMN(result, 0,
	                     {"{\"d\":3,\"e\":4}", "{\"a\":0,\"b\":1,\"c\":2}", Value(), "{\"a\":0,\"b\":1,\"c\":2}",
	                      "{\"c\":2,\"d\":3}", "{\"b\":1,\"c\":2,\"d\":3}", "{\"d\":3,\"e\":4}"}));

	// the arrays of the shared lists are themselves linked into the struct of each row
	result = con.Query("SELECT to_json({'l': list_child_range([0, 1, 2, 3, 4], o, n)})" + bounds);
	REQUIRE(CHECK_COLUMN(result, 0,
	                     {"{\"l\":[3,4]}", "{\"l\":[0,1,2]}", "{\"l\":null}", "{\"l\":[0,1,2]}", "{\"l\":[2,3]}",
	                      "{\"l\":[1,2,3]}", "{\"l\":[3,4]}"}));
}
