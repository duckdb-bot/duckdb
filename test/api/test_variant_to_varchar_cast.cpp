#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/main/config.hpp"

using namespace duckdb;

//! Writes 'n<value>', and NULL for 2
static bool RegisteredBigintToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto values = source.Values<int64_t>();
	auto writer = FlatVector::Writer<string_t>(result, count);
	for (auto entry : values) {
		if (!entry.IsValid() || entry.GetValue() == 2) {
			writer.WriteNull();
			continue;
		}
		auto text = "n" + to_string(entry.GetValue());
		writer.WriteValue(string_t(text.c_str(), UnsafeNumericCast<uint32_t>(text.size())));
	}
	return true;
}

TEST_CASE("VARIANT values cast to VARCHAR with a registered cast of their primitive values", "[api]") {
	DuckDB db;
	Connection con(db);
	auto &casts = DBConfig::GetConfig(*con.context).GetCastFunctions();
	casts.RegisterCastFunction(LogicalType::BIGINT, LogicalType::VARCHAR, BoundCastInfo(RegisteredBigintToVarcharCast));

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE converted AS SELECT * FROM (VALUES (1, [1::BIGINT, 4]::VARIANT), (2, "
	                          "{'k': 5::BIGINT, 'l': 'x'}::VARIANT), (3, 6::BIGINT::VARIANT)) t(i, v)"));
	auto result = con.Query("SELECT v::VARCHAR FROM converted ORDER BY i");
	REQUIRE(CHECK_COLUMN(result, 0, {"[n1, n4]", "{'k': n5, 'l': x}", "n6"}));
	result = con.Query("SELECT v::VARCHAR[] FROM converted WHERE i = 1");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::LIST(LogicalType::VARCHAR, {"n1", "n4"})}));

	// the registered cast returns NULL
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE nulls AS SELECT * FROM (VALUES (1, [1::BIGINT, 2]::VARIANT), (2, "
	                          "{'k': 2::BIGINT}::VARIANT), (3, 2::BIGINT::VARIANT), (4, 'a'::VARIANT), (5, "
	                          "[{'a': 4::BIGINT}::VARIANT, {'A': 5::BIGINT}::VARIANT]::VARIANT)) t(i, v)"));
	result = con.Query("SELECT v::VARCHAR FROM nulls ORDER BY i");
	REQUIRE(CHECK_COLUMN(result, 0, {"[n1, NULL]", "{'k': NULL}", Value(), "a", "[{'a': n4}, {'A': n5}]"}));
}
