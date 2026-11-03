#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

using namespace duckdb;

static void ArgumentCountExec(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value::BIGINT(NumericCast<int64_t>(args.ColumnCount())), count_t(args.size()));
}

static FunctionDescription Describe(LogicalType parameter_type, string description) {
	FunctionDescription result;
	result.parameter_types.push_back(std::move(parameter_type));
	result.description = std::move(description);
	return result;
}

// registers one overload per type, and returns the description duckdb_functions() shows for each
static vector<string> DescribeOverloads(Connection &con, const string &name, const vector<LogicalType> &types,
                                        vector<FunctionDescription> descriptions) {
	ScalarFunctionSet set {Identifier(name)};
	for (auto &type : types) {
		set.AddFunction(ScalarFunction({type}, LogicalType::BIGINT, ArgumentCountExec));
	}
	CreateScalarFunctionInfo info(set);
	info.descriptions = std::move(descriptions);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
	auto result = con.Query("SELECT parameter_types[1] || ' ' || coalesce(description, 'NULL') FROM duckdb_functions() "
	                        "WHERE function_name = '" +
	                        name + "' ORDER BY ALL");
	REQUIRE_NO_FAIL(*result);
	vector<string> described;
	for (idx_t row = 0; row < result->RowCount(); row++) {
		described.push_back(result->Collection().GetValue(0, row).ToString());
	}
	return described;
}

TEST_CASE("duckdb_functions picks the description of each overload's own parameter types", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto list = LogicalType::LIST(LogicalType::INTEGER);
	auto aliased_list = list.WithAlias("int_list");
	auto map = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::INTEGER);
	auto aliased_map = map.WithAlias("int_map");

	// an aliased container and the plain one each keep their own description, in either order
	vector<string> expected {"INTEGER[] plain list", "int_list aliased list"};
	REQUIRE(DescribeOverloads(con, "aliased_first", {aliased_list, list},
	                          {Describe(aliased_list, "aliased list"), Describe(list, "plain list")}) == expected);
	REQUIRE(DescribeOverloads(con, "plain_first", {aliased_list, list},
	                          {Describe(list, "plain list"), Describe(aliased_list, "aliased list")}) == expected);
	expected = {"MAP(VARCHAR, INTEGER) plain map", "int_map aliased map"};
	REQUIRE(DescribeOverloads(con, "aliased_map", {aliased_map, map},
	                          {Describe(aliased_map, "aliased map"), Describe(map, "plain map")}) == expected);

	// a nested ANY describes any element type, and an exact description wins over it
	expected = {"BIGINT[] any list", "INTEGER[] integer list", "VARCHAR NULL"};
	REQUIRE(DescribeOverloads(con, "generic_list", {LogicalType::LIST(LogicalType::BIGINT), list, LogicalType::VARCHAR},
	                          {Describe(LogicalType::LIST(LogicalType::ANY), "any list"),
	                           Describe(list, "integer list")}) == expected);

	// a STRUCT without fields, with or without type info, describes any STRUCT
	auto struct_type = LogicalType::STRUCT({{"a", LogicalType::INTEGER}});
	expected = {"STRUCT(a INTEGER) any struct", "VARCHAR text"};
	REQUIRE(DescribeOverloads(
	            con, "bare_struct", {struct_type, LogicalType::VARCHAR},
	            {Describe(LogicalTypeId::STRUCT, "any struct"), Describe(LogicalType::VARCHAR, "text")}) == expected);
	REQUIRE(DescribeOverloads(
	            con, "empty_struct", {struct_type, LogicalType::VARCHAR},
	            {Describe(LogicalType::STRUCT({}), "any struct"), Describe(LogicalType::VARCHAR, "text")}) == expected);
}
