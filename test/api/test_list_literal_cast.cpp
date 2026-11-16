#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
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

//! The calls of a registered cast, counted by the test and shared by every bound copy of the cast
struct CastCallCounter : public BoundCastData {
	explicit CastCallCounter(atomic<idx_t> &calls_p) : calls(calls_p) {
	}

	atomic<idx_t> &calls;

public:
	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<CastCallCounter>(calls);
	}
};

//! Casts every row to the number of calls so far, so each evaluation of the cast yields a new value
static bool CountingVarcharToBigintCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto call = ++parameters.cast_data->Cast<CastCallCounter>().calls;
	auto writer = FlatVector::Writer<int64_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		writer.WriteValue(NumericCast<int64_t>(call));
	}
	return true;
}

TEST_CASE("A list element whose registered cast is volatile is cast where the list is evaluated", "[api]") {
	atomic<idx_t> cast_calls {0};
	DuckDB db;
	Connection con(db);
	BoundCastInfo volatile_cast(CountingVarcharToBigintCast, make_uniq<CastCallCounter>(cast_calls));
	volatile_cast.SetStability(FunctionStability::VOLATILE);
	auto &casts = DBConfig::GetConfig(*con.context).GetCastFunctions();
	casts.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::BIGINT, std::move(volatile_cast));

	// binding the list does not run the cast, each execution runs it once
	auto prepared = con.Prepare("SELECT [2147483648, 'x']");
	REQUIRE(!prepared->HasError());
	REQUIRE(cast_calls.load() == 0);
	auto result = prepared->Execute();
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {Value::LIST({Value::BIGINT(2147483648), Value::BIGINT(1)})}));
	result = prepared->Execute();
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {Value::LIST({Value::BIGINT(2147483648), Value::BIGINT(2)})}));
	REQUIRE(cast_calls.load() == 2);
}
