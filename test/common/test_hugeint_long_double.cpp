#include "catch.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"

#include <cmath>
#include <limits>

using namespace duckdb;

template <class REAL_T>
static string ConvertToHugeint(REAL_T value) {
	hugeint_t result;
	return Hugeint::TryConvert(value, result) ? result.ToString() : "error";
}

template <class REAL_T>
static string ConvertToUhugeint(REAL_T value) {
	uhugeint_t result;
	return Uhugeint::TryConvert(value, result) ? result.ToString() : "error";
}

TEST_CASE("Convert a long double to HUGEINT and UHUGEINT without losing digits", "[hugeint]") {
	// these inputs require at least 64 significand bits
	if (std::numeric_limits<long double>::digits >= 64) {
		REQUIRE(ConvertToHugeint(18446744073709551615.0L) == "18446744073709551615");
		REQUIRE(ConvertToUhugeint(18446744073709551615.0L) == "18446744073709551615");
		REQUIRE(ConvertToHugeint(18446744073709551618.0L) == "18446744073709551618");
		REQUIRE(ConvertToHugeint(184467440737095551616.0L) == "184467440737095551616");
		REQUIRE(ConvertToHugeint(-184467440737095551616.0L) == "-184467440737095551616");
		REQUIRE(ConvertToUhugeint(184467440737095551616.0L) == "184467440737095551616");
		REQUIRE(ConvertToHugeint(std::ldexp(1.0L, 127) - std::ldexp(1.0L, 64)) ==
		        "170141183460469231713240559642174554112");
	}
	// exact in every floating point type
	REQUIRE(ConvertToHugeint(std::ldexp(1.0L, 64)) == "18446744073709551616");
	REQUIRE(ConvertToUhugeint(std::ldexp(1.0L, 100)) == "1267650600228229401496703205376");
	REQUIRE(ConvertToHugeint(1.9L) == "1");
	REQUIRE(ConvertToHugeint(-1.9L) == "-1");
	REQUIRE(ConvertToHugeint(std::ldexp(1.0, 127) - std::ldexp(1.0, 74)) == "170141183460469212842221372237303250944");
	REQUIRE(ConvertToHugeint(std::ldexp(-1.0, 127) + std::ldexp(1.0, 74)) ==
	        "-170141183460469212842221372237303250944");
	REQUIRE(ConvertToUhugeint(std::ldexp(1.0, 128) - std::ldexp(1.0, 75)) == "340282366920938425684442744474606501888");
	REQUIRE(ConvertToHugeint(1.9) == "1");
	REQUIRE(ConvertToHugeint(1.9f) == "1");
	// out of range or not finite
	REQUIRE(ConvertToHugeint(std::ldexp(1.0L, 127)) == "error");
	REQUIRE(ConvertToHugeint(std::ldexp(1.0, 127)) == "error");
	REQUIRE(ConvertToUhugeint(std::ldexp(1.0L, 128)) == "error");
	REQUIRE(ConvertToUhugeint(-1.0L) == "error");
	REQUIRE(ConvertToHugeint(std::numeric_limits<long double>::quiet_NaN()) == "error");
	REQUIRE(ConvertToHugeint(std::numeric_limits<long double>::infinity()) == "error");
	REQUIRE(ConvertToUhugeint(std::numeric_limits<long double>::quiet_NaN()) == "error");
	REQUIRE(ConvertToHugeint(std::numeric_limits<double>::quiet_NaN()) == "error");
}
