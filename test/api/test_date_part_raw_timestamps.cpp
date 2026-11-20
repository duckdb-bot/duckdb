#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/appender.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

// a raw TIMESTAMP_MS value this large has no TIMESTAMP equivalent, so the widening fails and TRY turns that into NULL
TEST_CASE("Test time parts of raw TIMESTAMP_MS values beyond the TIMESTAMP range", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE raw_ms(ts TIMESTAMP_MS)"));
	{
		Appender appender(con, "raw_ms");
		appender.AppendRow(Value::TIMESTAMPMS(timestamp_ms_t(0)));
		appender.AppendRow(Value::TIMESTAMPMS(timestamp_ms_t(9223372036854776)));
	}
	auto result = con.Query("SELECT TRY(hour(ts)) FROM raw_ms ORDER BY ts");
	REQUIRE(CHECK_COLUMN(result, 0, {0, Value()}));
	REQUIRE_FAIL(con.Query("SELECT hour(ts) FROM raw_ms"));
	result = con.Query("SELECT TRY(date_part('minute', ts)) FROM raw_ms ORDER BY ts");
	REQUIRE(CHECK_COLUMN(result, 0, {0, Value()}));
	result = con.Query("SELECT TRY(year(ts)), TRY(julian(ts)) FROM raw_ms ORDER BY ts");
	REQUIRE(CHECK_COLUMN(result, 0, {1970, Value()}));
	REQUIRE(CHECK_COLUMN(result, 1, {2440588.0, Value()}));
	result = con.Query("SELECT hour(ts) FROM raw_ms WHERE ts < TIMESTAMP_MS '1970-01-02'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));

	// the widening of seconds overflows in both directions
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE raw_s(ts TIMESTAMP_S)"));
	{
		Appender appender(con, "raw_s");
		appender.AppendRow(Value::TIMESTAMPSEC(timestamp_sec_t(-9223372036855)));
		appender.AppendRow(Value::TIMESTAMPSEC(timestamp_sec_t(0)));
		appender.AppendRow(Value::TIMESTAMPSEC(timestamp_sec_t(9223372036855)));
	}
	result = con.Query("SELECT TRY(year(ts)), TRY(julian(ts)) FROM raw_s ORDER BY ts");
	REQUIRE(CHECK_COLUMN(result, 0, {Value(), 1970, Value()}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value(), 2440588.0, Value()}));
	result = con.Query("SELECT year(ts) FROM raw_s");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not convert Timestamp to higher precision"));
}
