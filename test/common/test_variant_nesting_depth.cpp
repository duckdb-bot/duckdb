#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/types/value.hpp"

using namespace duckdb;

TEST_CASE("A VARIANT nested deeper than the limit does not convert to a Value", "[variant]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = con.Query("WITH RECURSIVE r(d, v) AS (SELECT 1, [1]::VARIANT UNION ALL "
	                        "SELECT d + 1, [v]::VARIANT FROM r WHERE d < 129) "
	                        "SELECT v FROM r WHERE d IN (128, 129) ORDER BY d");
	REQUIRE_NO_FAIL(*result);
	auto shallow = result->Collection().GetValue(0, 0);
	auto deep = result->Collection().GetValue(0, 1);

	REQUIRE(VariantValue::GetValue(shallow).type().id() == LogicalTypeId::LIST);
	REQUIRE_THROWS_WITH(VariantValue::GetValue(deep), Catch::Contains("nested deeper than 128 levels"));

	// the SQL of the shallow value reads back, the SQL of the deep one fails when it is executed
	auto shallow_sql = con.Query("SELECT " + shallow.ToSQLString() + " = " + shallow.ToSQLString());
	REQUIRE(CHECK_COLUMN(shallow_sql, 0, {true}));
	auto deep_sql = con.Query("SELECT " + deep.ToSQLString());
	REQUIRE(deep_sql->HasError());
	REQUIRE(StringUtil::Contains(deep_sql->GetError(), "nested deeper than 128 levels"));
}
