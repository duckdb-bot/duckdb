#include "catch.hpp"
#include "duckdb/common/bit_utils.hpp"

using namespace duckdb;

TEST_CASE("CountZeros of 64-bit values with bits above the lower 32", "[bit_utils]") {
	REQUIRE(CountZeros<uint64_t>::Leading(0) == 64);
	REQUIRE(CountZeros<uint64_t>::Trailing(0) == 64);
	REQUIRE(CountZeros<uint64_t>::Leading(NumericLimits<uint64_t>::Maximum()) == 0);
	REQUIRE(CountZeros<uint64_t>::Trailing(NumericLimits<uint64_t>::Maximum()) == 0);
	for (idx_t bit = 0; bit < 64; bit++) {
		auto value = uint64_t(1) << bit;
		REQUIRE(CountZeros<uint64_t>::Leading(value) == 63 - bit);
		REQUIRE(CountZeros<uint64_t>::Trailing(value) == bit);
		REQUIRE(CountZeros<uint64_t>::Leading(value | 1) == 63 - bit);
		REQUIRE(CountZeros<uint64_t>::Trailing(value | (uint64_t(1) << 63)) == bit);
	}
	REQUIRE(CountZeros<uint64_t>::Leading(0x0000000180000000ULL) == 31);
	REQUIRE(CountZeros<uint64_t>::Trailing(0x0000000180000000ULL) == 31);
}
