#include "catch.hpp"
#include "duckdb/main/appender.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

// raw TIMESTAMP_MS and TIMESTAMP_S values that no SQL literal reaches, so the statistics of epoch_ms over them are
// only testable through the API
TEST_CASE("Test epoch_ms statistics over raw TIMESTAMP_MS and TIMESTAMP_S values", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	// INT64_MIN counts as finite, the value above it is the negative infinity sentinel
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE raw_ms(ts TIMESTAMP_MS)"));
	{
		Appender appender(con, "raw_ms");
		appender.AppendRow(Value::TIMESTAMPMS(timestamp_ms_t(NumericLimits<int64_t>::Minimum())));
		appender.AppendRow(Value::TIMESTAMPMS(timestamp_ms_t(NumericLimits<int64_t>::Minimum() + 1)));
		appender.AppendRow(Value::TIMESTAMPMS(timestamp_ms_t(0)));
	}
	auto result = con.Query("SELECT epoch_ms(ts) FROM raw_ms ORDER BY ts");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(NumericLimits<int64_t>::Minimum()), Value(), 0}));
	result = con.Query("SELECT count(epoch_ms(ts)) FROM raw_ms");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	result = con.Query("SELECT count(*) FROM raw_ms WHERE epoch_ms(ts) IS NULL");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	// a seconds value whose milliseconds overflow fails, and TRY turns that into NULL, also when the optimizer
	// evaluates the bounds of the column
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE raw_s(ts TIMESTAMP_S)"));
	{
		Appender appender(con, "raw_s");
		appender.AppendRow(Value::TIMESTAMPSEC(timestamp_sec_t(0)));
		appender.AppendRow(Value::TIMESTAMPSEC(timestamp_sec_t(9223372036854776)));
	}
	result = con.Query("SELECT TRY(epoch_ms(ts)) FROM raw_s WHERE ts > TIMESTAMP_S '1970-01-01'");
	REQUIRE(CHECK_COLUMN(result, 0, {Value()}));
	REQUIRE_FAIL(con.Query("SELECT epoch_ms(ts) FROM raw_s WHERE ts > TIMESTAMP_S '1970-01-01'"));
	result = con.Query("SELECT epoch_ms(ts) FROM raw_s WHERE ts < TIMESTAMP_S '1970-01-02'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}
