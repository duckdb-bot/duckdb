#include "catch.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

// Evaluates `SELECT <expr>` and returns the resulting scalar Value.
static Value EvalScalar(Connection &con, const string &expr) {
	auto result = con.Query("SELECT " + expr);
	REQUIRE_NO_FAIL(*result);
	return result->Collection().GetValue(0, 0);
}

// Asserts the round-trip contract of Value::ToSQLString(): rendering a Value to
// SQL and re-parsing it yields a Value with the same type and contents.
static void RequireRoundTrip(Connection &con, const string &expr) {
	auto original = EvalScalar(con, expr);
	auto sql = original.ToSQLString();
	INFO("expr=" << expr << " ToSQLString=" << sql);
	auto roundtripped = EvalScalar(con, sql);
	REQUIRE(roundtripped.type() == original.type());
	REQUIRE(roundtripped.ToString() == original.ToString());
}

TEST_CASE("Value::ToSQLString round-trips MAP values", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	// flat string-keyed map
	RequireRoundTrip(con, "MAP {'a': 'b', 'c': 'd'}");
	// integer-keyed / integer-valued map
	RequireRoundTrip(con, "MAP {1: 10, 2: 20}");
	// empty map needs an explicit cast to keep its element types
	RequireRoundTrip(con, "MAP {}::MAP(VARCHAR, VARCHAR)");
	// nested map
	RequireRoundTrip(con, "MAP {'a': MAP {'b': 1}}");
	// map with LIST values
	RequireRoundTrip(con, "MAP {'a': [1, 2, 3]}");
	// map with STRUCT values
	RequireRoundTrip(con, "MAP {'a': {'x': 1, 'y': 2}}");
	// map with a NULL value
	RequireRoundTrip(con, "MAP {'k': NULL}::MAP(VARCHAR, VARCHAR)");
	// keys/values that require single-quote escaping
	RequireRoundTrip(con, "MAP {'it''s': 'a''b'}");
	// the explicit cast also preserves narrower integer subtypes
	RequireRoundTrip(con, "MAP {1::SMALLINT: 2::SMALLINT}");
}

TEST_CASE("Value::ToSQLString round-trips STRUCT, BIT and UNION values", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	SECTION("STRUCT key containing a quote") {
		RequireRoundTrip(con, "{'it''s': 42}");
	}
	SECTION("BIT value") {
		RequireRoundTrip(con, "'101'::BIT");
	}
	SECTION("UNION member types") {
		RequireRoundTrip(con, "union_value(i := 42::SMALLINT)::UNION(i SMALLINT, s VARCHAR)");
	}
}

TEST_CASE("Value::ToSQLString round-trips typed VARIANT payloads", "[api][value_sql]") {
	DuckDB db;
	Connection con(db);
	duckdb::vector<string> payloads {"NULL",
	                                 "true",
	                                 "false",
	                                 "'hello'",
	                                 "'it''s a duck'",
	                                 "''",
	                                 "'🦆'",
	                                 "chr(0)",
	                                 "'a' || chr(0) || 'b'",
	                                 "chr(0) || chr(0)",
	                                 "-7::TINYINT",
	                                 "-7::SMALLINT",
	                                 "-7::INTEGER",
	                                 "-7::BIGINT",
	                                 "-7::HUGEINT",
	                                 "7::UTINYINT",
	                                 "7::USMALLINT",
	                                 "7::UINTEGER",
	                                 "7::UBIGINT",
	                                 "7::UHUGEINT",
	                                 "1.25::FLOAT",
	                                 "1.25::DOUBLE",
	                                 "'NaN'::DOUBLE",
	                                 "'Infinity'::DOUBLE",
	                                 "'-Infinity'::FLOAT",
	                                 "12.34::DECIMAL(9,2)",
	                                 "12.34::DECIMAL(38,2)",
	                                 "1234567890123456789012345678901234567890::BIGNUM",
	                                 "DATE '2025-01-02'",
	                                 "TIME '12:34:56'",
	                                 "'12:34:56.123456789'::TIME_NS",
	                                 "'12:34:56+01'::TIMETZ",
	                                 "TIMESTAMP '2025-01-02 12:34:56'",
	                                 "'2025-01-02 12:34:56'::TIMESTAMP_S",
	                                 "'2025-01-02 12:34:56.123'::TIMESTAMP_MS",
	                                 "'2025-01-02 12:34:56.123456789'::TIMESTAMP_NS",
	                                 "'2025-01-02 12:34:56+01'::TIMESTAMPTZ",
	                                 "'2025-01-02 12:34:56.123456789+01'::TIMESTAMPTZ_NS",
	                                 "INTERVAL '1 day 2 seconds'",
	                                 "'00000000-0000-0000-0000-000000000001'::UUID",
	                                 "'a\\x00b'::BLOB",
	                                 "'a''b'::BLOB",
	                                 "'10101'::BIT",
	                                 "'POINT (1 2)'::GEOMETRY",
	                                 "[]::SMALLINT[]",
	                                 "[1::SMALLINT,2::SMALLINT,NULL]",
	                                 "[1::SMALLINT::VARIANT,'x'::VARIANT,NULL::VARIANT]",
	                                 "[NULL,NULL]",
	                                 "{}",
	                                 "{'empty': {}, 'n': NULL}",
	                                 "{'it''s': 7::SMALLINT, 'nested': ['x',NULL]}",
	                                 "{'slash\\key': {'🦆': 12.34::DECIMAL(9,2)}}",
	                                 "{'items': [1::SMALLINT::VARIANT, {'key': 'value'}::VARIANT]}"};
	for (auto &payload : payloads) {
		INFO(payload);
		auto expression = "(" + payload + ")::VARIANT";
		auto original = EvalScalar(con, expression);
		auto rendered = original.ToSQLString();
		auto rebound = EvalScalar(con, rendered);
		REQUIRE(rebound.type() == original.type());
		REQUIRE(EvalScalar(con, "(" + expression + ") IS NOT DISTINCT FROM (" + rendered + ")") ==
		        Value::BOOLEAN(true));
		REQUIRE(EvalScalar(con, "variant_typeof(" + rendered + ")") ==
		        EvalScalar(con, "variant_typeof(" + expression + ")"));
	}
	for (auto &expression : {"[1::SMALLINT::VARIANT,'x'::VARIANT]", "{'v': 7::SMALLINT::VARIANT}",
	                         "MAP {'v': 7::SMALLINT::VARIANT}", "union_value(v := 7::SMALLINT::VARIANT)"}) {
		RequireRoundTrip(con, expression);
	}
	for (auto &expression : {"[1::SMALLINT::VARIANT,'x'::VARIANT]::VARIANT", "{'v': 7::SMALLINT}::VARIANT"}) {
		auto rendered = EvalScalar(con, expression).ToSQLString();
		auto selector = expression[0] == '[' ? "[1]" : ".v";
		REQUIRE(EvalScalar(con, "variant_typeof((" + rendered + ")" + selector + ")") == Value("INT16"));
	}
	string deep = "{'it''s': [7::SMALLINT::VARIANT, 12.34::DECIMAL(9,2)::VARIANT, 'a''b'::VARIANT]}::VARIANT";
	string path;
	for (idx_t depth = 0; depth < 12; depth++) {
		deep = "{'nested': [" + deep + ", NULL::VARIANT]}::VARIANT";
		path += ".nested[1]";
	}
	auto rendered = EvalScalar(con, deep).ToSQLString();
	REQUIRE(EvalScalar(con, "(" + deep + ") IS NOT DISTINCT FROM (" + rendered + ")") == Value::BOOLEAN(true));
	for (auto index : {"[1]", "[2]", "[3]"}) {
		auto child_path = path + "['it''s']" + index;
		REQUIRE(EvalScalar(con, "variant_typeof((" + deep + ")" + child_path + ")") ==
		        EvalScalar(con, "variant_typeof((" + rendered + ")" + child_path + ")"));
	}
}

TEST_CASE("Value::ToSQLString renders nested VARIANT values once", "[api][value_sql]") {
	DuckDB db;
	Connection con(db);
	REQUIRE(EvalScalar(con, "[1::SMALLINT, NULL]::VARIANT").ToSQLString() ==
	        "CAST([CAST(CAST(1 AS SMALLINT) AS VARIANT), NULL::VARIANT] AS VARIANT)");
	REQUIRE(EvalScalar(con, "{'b': 'x', 'a': [2]}::VARIANT").ToSQLString() ==
	        "CAST({'a': CAST([CAST(CAST(2 AS INTEGER) AS VARIANT)] AS VARIANT), 'b': CAST(CAST('x' AS VARCHAR) AS "
	        "VARIANT)} AS VARIANT)");
	// heterogeneous arrays around a scalar 127 levels deep, the deepest value VariantVisitor accepts
	string deep = "1::VARIANT";
	for (idx_t depth = 0; depth < 127; depth++) {
		deep = "['x'::VARIANT, (" + deep + ")]::VARIANT";
	}
	auto rendered = EvalScalar(con, deep).ToSQLString();
	REQUIRE(EvalScalar(con, "(" + deep + ") IS NOT DISTINCT FROM (" + rendered + ")") == Value::BOOLEAN(true));
}

// Casts JSON that nests an empty array in 'depth' levels, each opened by 'open' and closed by 'close', to VARIANT
// during execution, since the optimizer names a constant VARIANT with Value::ToSQLString
static string DeepJSONVariant(const string &open, const string &close, idx_t depth) {
	return "(SELECT (repeat('" + open + "', i) || '[]' || repeat('" + close + "', i))::JSON::VARIANT FROM range(" +
	       to_string(depth) + ", " + to_string(depth + 1) + ") t(i))";
}

TEST_CASE("Value::ToSQLString fails on JSON-derived VARIANT values deeper than VariantVisitor accepts",
          "[api][value_sql]") {
	DuckDB db;
	Connection con(db);
	if (!db.instance->ExtensionIsLoaded("json")) {
		WARN("JSON extension required for deep VARIANT coverage");
		return;
	}
	// an empty array inside 127 arrays or objects is the deepest value that renders
	for (auto &original : {DeepJSONVariant("[", "]", 127), DeepJSONVariant("{\"a\": ", "}", 127)}) {
		auto rendered = EvalScalar(con, original).ToSQLString();
		REQUIRE(EvalScalar(con, "(" + original + ")::JSON = (" + rendered + ")::JSON") == Value::BOOLEAN(true));
	}
	// an object whose only key is empty renders as a list of key and value pairs instead
	REQUIRE_NOTHROW(EvalScalar(con, DeepJSONVariant("{\"\": ", "}", 127)).ToSQLString());
	// deeper values fail without recursing once per level
	for (idx_t depth : {idx_t(128), idx_t(50000)}) {
		for (auto &deep : {DeepJSONVariant("[", "]", depth), DeepJSONVariant("{\"a\": ", "}", depth),
		                   DeepJSONVariant("{\"\": ", "}", depth)}) {
			INFO(deep.substr(0, 30) << " " << depth);
			REQUIRE_THROWS_WITH(EvalScalar(con, deep).ToSQLString(),
			                    Catch::Matchers::Contains("VARIANT exceeds maximum recursion depth of 128"));
		}
	}
}

TEST_CASE("Value::ToSQLString round-trips JSON-derived supported VARIANT objects", "[api][value_sql]") {
	DuckDB db;
	Connection con(db);
	if (!db.instance->ExtensionIsLoaded("json")) {
		WARN("JSON extension required for empty VARIANT key coverage");
		return;
	}
	for (auto json : {"{}", "{\"a\":1}", "{\"outer\":{\"x\":2}}", "{\"\\u0000\":1}"}) {
		auto original = "'" + string(json) + "'::JSON::VARIANT";
		auto rendered = EvalScalar(con, original).ToSQLString();
		REQUIRE(EvalScalar(con, "(" + original + ") IS NOT DISTINCT FROM (" + rendered + ")") == Value::BOOLEAN(true));
		for (auto function : {"variant_typeof", "variant_keys"}) {
			REQUIRE(EvalScalar(con, string(function) + "(" + original + ")") ==
			        EvalScalar(con, string(function) + "(" + rendered + ")"));
		}
	}
}

TEST_CASE("Value::ToSQLString renders VARIANT objects whose only key is empty", "[api][value_sql]") {
	DuckDB db;
	Connection con(db);
	if (!db.instance->ExtensionIsLoaded("json")) {
		WARN("JSON extension required for empty VARIANT key coverage");
		return;
	}
	// at the root, as a field next to another one, and inside an array
	duckdb::vector<std::pair<string, bool>> wrappers {
	    {"%s", false}, {"{\"a\":%s,\"b\":[1,null]}", true}, {"[%s,{\"c\":%s}]", false}};
	for (auto json : {"{\"\":1}", "{\"\":null}", "{\"\":[1,\"x\"]}", "{\"\":{\"\":{\"b\":2}}}"}) {
		for (auto &wrapper : wrappers) {
			auto original = "'" + StringUtil::Replace(wrapper.first, "%s", json) + "'::JSON::VARIANT";
			INFO(original);
			auto rendered = EvalScalar(con, original).ToSQLString();
			REQUIRE(!EvalScalar(con, "variant_typeof(" + rendered + ")").IsNull());
			if (wrapper.second) {
				REQUIRE(EvalScalar(con, "(" + original + ").b IS NOT DISTINCT FROM (" + rendered + ").b") ==
				        Value::BOOLEAN(true));
			}
		}
	}
}

TEST_CASE("Value::ToSQLString preserves negative floating zero", "[api][value_sql]") {
	DuckDB db;
	Connection con(db);
	for (auto type : {"FLOAT", "DOUBLE"}) {
		for (auto suffix : {"", "::VARIANT"}) {
			auto expression = "'-0.0'::" + string(type) + suffix;
			auto original = EvalScalar(con, expression);
			auto rendered = original.ToSQLString();
			REQUIRE(EvalScalar(con, rendered).type() == original.type());
			REQUIRE(EvalScalar(con, "1.0 / (" + rendered + ")::DOUBLE") ==
			        EvalScalar(con, "1.0 / (" + expression + ")::DOUBLE"));
		}
	}
}
