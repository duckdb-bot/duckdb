#include "catch.hpp"
#include "duckdb/common/bit_utils.hpp"

using namespace duckdb;

// the debug check against the compiler builtins also holds where long has 32 bits
TEST_CASE("CountZeros of 64-bit values with bits above the lower 32", "[bit_utils]") {
	REQUIRE(CountZeros<uint64_t>::Leading(0x0000000180000000ULL) == 31);
	REQUIRE(CountZeros<uint64_t>::Trailing(0x0000000180000000ULL) == 31);
	REQUIRE(CountZeros<uint64_t>::Leading(0x8000000000000000ULL) == 0);
	REQUIRE(CountZeros<uint64_t>::Trailing(0x8000000000000000ULL) == 63);
	REQUIRE(CountZeros<uint64_t>::Leading(0x0000000100000001ULL) == 31);
	REQUIRE(CountZeros<uint64_t>::Trailing(0x0000000100000001ULL) == 0);
}
