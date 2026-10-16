#include "catch.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"

#include <limits>

using namespace duckdb;

TEST_CASE("Convert a long double to HUGEINT and UHUGEINT without losing digits", "[hugeint]") {
	hugeint_t hugeint;
	uhugeint_t uhugeint;
	// exact with an 80-bit or 128-bit long double, which holds 64 bits of mantissa
	if (std::numeric_limits<long double>::digits >= 64) {
		long double max_limb = 18446744073709551615.0L;
		REQUIRE(Hugeint::TryConvert(max_limb, hugeint));
		REQUIRE(hugeint.ToString() == "18446744073709551615");
		REQUIRE(Uhugeint::TryConvert(max_limb, uhugeint));
		REQUIRE(uhugeint.ToString() == "18446744073709551615");

		long double value = 184467440737095551616.0L;
		REQUIRE(Hugeint::TryConvert(value, hugeint));
		REQUIRE(hugeint.ToString() == "184467440737095551616");
		REQUIRE(Hugeint::TryConvert(-value, hugeint));
		REQUIRE(hugeint.ToString() == "-184467440737095551616");
		REQUIRE(Uhugeint::TryConvert(value, uhugeint));
		REQUIRE(uhugeint.ToString() == "184467440737095551616");
	}
	// powers of two are exact in every floating point type
	REQUIRE(Hugeint::TryConvert(std::ldexp(1.0L, 100), hugeint));
	REQUIRE(hugeint.ToString() == "1267650600228229401496703205376");
	REQUIRE(Hugeint::TryConvert(std::ldexp(1.0, 64), hugeint));
	REQUIRE(hugeint.ToString() == "18446744073709551616");
	REQUIRE(Uhugeint::TryConvert(std::ldexp(1.0, 127), uhugeint));
	REQUIRE(uhugeint.ToString() == "170141183460469231731687303715884105728");
}
