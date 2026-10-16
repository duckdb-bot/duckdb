#include "catch.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

atomic<bool> overload_registered {false};

void AddOneFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<int32_t, int32_t>(args.data[0], result, args.size(),
	                                         [](int32_t input) { return input + 1; });
}

void EchoFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(args.data[0]);
}

void FortyOneFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value::INTEGER(41), count_t(args.size()));
}

void RegisterFunction(DatabaseInstance &db, ScalarFunction function) {
	ExtensionInfo info {};
	ExtensionActiveLoad load_info {db, info, "overload_test", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(std::move(function));
}

// Adds an overload to outer_function while an argument of outer_function is bound, as autoloading an extension does
unique_ptr<FunctionData> RegisterOverloadBind(BindScalarFunctionInput &input) {
	if (!overload_registered.exchange(true)) {
		RegisterFunction(DatabaseInstance::GetDatabase(input.GetClientContext()),
		                 ScalarFunction("outer_function", {LogicalType::VARCHAR}, LogicalType::VARCHAR, EchoFunction));
	}
	return nullptr;
}

} // namespace

TEST_CASE("Test adding an overload of a function while one of its arguments is bound", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	overload_registered = false;
	RegisterFunction(*db.instance,
	                 ScalarFunction("outer_function", {LogicalType::INTEGER}, LogicalType::INTEGER, AddOneFunction));
	ScalarFunction inner("inner_function", {}, LogicalType::INTEGER, FortyOneFunction);
	inner.SetBindCallback(RegisterOverloadBind);
	RegisterFunction(*db.instance, std::move(inner));

	auto result = con.Query("SELECT outer_function(inner_function())");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE(overload_registered);
	result = con.Query("SELECT outer_function('duck'), outer_function(1)");
	REQUIRE(CHECK_COLUMN(result, 0, {"duck"}));
	REQUIRE(CHECK_COLUMN(result, 1, {2}));
}
