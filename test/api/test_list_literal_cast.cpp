#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/main/config.hpp"

using namespace duckdb;

static bool ThrowingVarcharToBigintCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	throw InvalidInputException("refused VARCHAR to BIGINT cast");
}

TEST_CASE("A list element whose registered cast throws fails only where the list is evaluated", "[api]") {
	DuckDB db;
	Connection con(db);
	auto &casts = DBConfig::GetConfig(*con.context).GetCastFunctions();
	casts.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::BIGINT, BoundCastInfo(ThrowingVarcharToBigintCast));

	auto result = con.Query("SELECT CASE WHEN i IS NULL THEN [i, 'bad'] END FROM range(3) t(i)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {Value(), Value(), Value()}));

	auto prepared = con.Prepare("SELECT [i, 'bad'] FROM range(3) t(i)");
	REQUIRE(!prepared->HasError());
	auto executed = prepared->Execute();
	REQUIRE(executed->HasError());
	REQUIRE(StringUtil::Contains(executed->GetError(), "refused VARCHAR to BIGINT cast"));

	// a list of constants only, which binding evaluates
	result = con.Query("SELECT CASE WHEN i IS NULL THEN [2147483648, 'bad'] END FROM range(3) t(i)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {Value(), Value(), Value()}));

	prepared = con.Prepare("SELECT [2147483648, 'bad']");
	REQUIRE(!prepared->HasError());
	executed = prepared->Execute();
	REQUIRE(executed->HasError());
	REQUIRE(StringUtil::Contains(executed->GetError(), "refused VARCHAR to BIGINT cast"));
}
