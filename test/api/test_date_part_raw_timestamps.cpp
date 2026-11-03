#include "catch.hpp"
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
	result = con.Query("SELECT hour(ts) FROM raw_ms WHERE ts < TIMESTAMP_MS '1970-01-02'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}
