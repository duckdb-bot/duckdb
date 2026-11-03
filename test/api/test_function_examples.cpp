#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"

using namespace duckdb;

TEST_CASE("The documented examples of the VARIANT functions run", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	const char *documented_examples[] = {VariantArrayLengthFun::Example,   VariantContainsFun::Example,
	                                     VariantExistsFun::Example,        VariantExtractFun::Example,
	                                     VariantExtractStringFun::Example, VariantNormalizeFun::Example,
	                                     VariantComparatorFun::Example,    VariantTypeFun::Example,
	                                     VariantTypeofFun::Example,        VariantKeysFun::Example};
	for (auto examples : documented_examples) {
		idx_t example_count = 0;
		// one entry per overload, each with one or more examples
		for (auto &overload_examples : StringUtil::Split(examples, '\1')) {
			for (auto &example : StringUtil::Split(overload_examples, '\2')) {
				INFO(example);
				auto result = con.Query("SELECT " + example);
				REQUIRE_NO_FAIL(*result);
				REQUIRE(result->RowCount() == 1);
				example_count++;
			}
		}
		REQUIRE(example_count > 0);
	}
}
