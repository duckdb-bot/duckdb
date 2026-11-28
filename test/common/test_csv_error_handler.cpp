#include "catch.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_error.hpp"

using namespace duckdb; // NOLINT

namespace {
//! The error the handler raises, or an invalid error when it raises nothing
template <class RAISE>
ErrorData RaisedError(RAISE &&raise) {
	try {
		raise();
	} catch (std::exception &ex) {
		return ErrorData(ex);
	}
	return ErrorData();
}
} // namespace

TEST_CASE("The CSV error handler raises the earliest stored error with its own exception class", "[csv]") {
	CSVErrorHandler handler;
	// three scan ranges of ten lines each
	const LinesPerBoundary second_range(1, 5);
	const LinesPerBoundary third_range(2, 0);

	SECTION("a cast error behind which another range stored its error first") {
		handler.Error(CSVError("a row with too many columns", TOO_MANY_COLUMNS, third_range));
		handler.Error(CSVError("a value that does not convert", CAST_ERROR, second_range));
		// the lines of the ranges before the errors are not known yet
		REQUIRE(!RaisedError([&]() { handler.ErrorIfNeeded(); }).HasError());
		handler.Insert(0, 10);
		handler.Insert(1, 10);
		auto error = RaisedError([&]() { handler.ErrorIfNeeded(); });
		REQUIRE(error.HasError());
		REQUIRE(StringUtil::Contains(error.RawMessage(), "CSV Error on Line: 16"));
		REQUIRE(StringUtil::Contains(error.RawMessage(), "a value that does not convert"));
		REQUIRE(error.Type() == ExceptionType::CONVERSION);
	}
	SECTION("a row with too many columns behind which another range stored its error first") {
		handler.Error(CSVError("a value that does not convert", CAST_ERROR, third_range));
		handler.Error(CSVError("a row with too many columns", TOO_MANY_COLUMNS, second_range));
		handler.Insert(0, 10);
		handler.Insert(1, 10);
		auto error = RaisedError([&]() { handler.ErrorIfAny(); });
		REQUIRE(error.HasError());
		REQUIRE(StringUtil::Contains(error.RawMessage(), "CSV Error on Line: 16"));
		REQUIRE(StringUtil::Contains(error.RawMessage(), "a row with too many columns"));
		REQUIRE(error.Type() == ExceptionType::INVALID_INPUT);
	}
	SECTION("an error that can be raised at once while an earlier one is stored") {
		handler.Error(CSVError("a value that does not convert", CAST_ERROR, second_range));
		handler.Insert(0, 10);
		handler.Insert(1, 10);
		auto error = RaisedError(
		    [&]() { handler.Error(CSVError("a row with too many columns", TOO_MANY_COLUMNS, third_range)); });
		REQUIRE(error.HasError());
		REQUIRE(StringUtil::Contains(error.RawMessage(), "CSV Error on Line: 16"));
		REQUIRE(StringUtil::Contains(error.RawMessage(), "a value that does not convert"));
		REQUIRE(error.Type() == ExceptionType::CONVERSION);
	}
}
