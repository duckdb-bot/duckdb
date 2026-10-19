#include "catch.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

//! The overload that binding inner_function adds
unique_ptr<ScalarFunction> pending_overload;

void AddOneFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<int32_t, int32_t>(args.data[0], result, args.size(),
	                                         [](int32_t input) { return input + 1; });
}

void EchoFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(args.data[0]);
}

void DuckFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value("duck"), count_t(args.size()));
}

void RegisterFunction(DatabaseInstance &db, ScalarFunction function) {
	ExtensionLoader loader(db, "overload_test");
	loader.RegisterFunction(std::move(function));
}

// Adds the pending overload while a call that it overloads is bound, as autoloading an extension does
unique_ptr<FunctionData> RegisterOverloadBind(BindScalarFunctionInput &input) {
	if (pending_overload) {
		auto overload = std::move(*pending_overload);
		pending_overload.reset();
		RegisterFunction(DatabaseInstance::GetDatabase(input.GetClientContext()), std::move(overload));
	}
	return nullptr;
}

unique_ptr<ScalarFunction> EchoOverload(const string &name) {
	return make_uniq<ScalarFunction>(Identifier(name), vector<LogicalType> {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                 EchoFunction);
}

//! Registers outer_function(INTEGER), and inner_function() whose bind adds the pending overload
void RegisterTestFunctions(DuckDB &db) {
	RegisterFunction(*db.instance,
	                 ScalarFunction("outer_function", {LogicalType::INTEGER}, LogicalType::INTEGER, AddOneFunction));
	ScalarFunction inner("inner_function", {}, LogicalType::VARCHAR, DuckFunction);
	inner.SetBindCallback(RegisterOverloadBind);
	RegisterFunction(*db.instance, std::move(inner));
}

} // namespace

TEST_CASE("Test adding an overload of a function while one of its arguments is bound", "[api]") {
	DuckDB db(nullptr);
	RegisterTestFunctions(db);
	Connection con(db);

	// the call is bound with the overload that binding its argument adds
	pending_overload = EchoOverload("outer_function");
	auto result = con.Query("SELECT outer_function(inner_function())");
	REQUIRE(CHECK_COLUMN(result, 0, {"duck"}));
	REQUIRE(!pending_overload);
	result = con.Query("SELECT outer_function('duck'), outer_function(1)");
	REQUIRE(CHECK_COLUMN(result, 0, {"duck"}));
	REQUIRE(CHECK_COLUMN(result, 1, {2}));
}

TEST_CASE("Test adding an overload of a lambda function while its lambda is bound", "[api]") {
	DuckDB db(nullptr);
	RegisterTestFunctions(db);
	Connection con(db);

	pending_overload = EchoOverload("list_transform");
	auto result = con.Query("SELECT list_transform([1, 2], lambda x: x + length(inner_function()))::VARCHAR");
	REQUIRE(CHECK_COLUMN(result, 0, {"[5, 6]"}));
	REQUIRE(!pending_overload);
}

TEST_CASE("Test adding an overload of a function while an arrow is bound as a lambda", "[api]") {
	DuckDB db(nullptr);
	RegisterTestFunctions(db);
	Connection con(db);

	// an arrow that does not bind as a lambda is bound again as a JSON operator
	pending_overload = EchoOverload("list_filter");
	auto result = con.Query("SELECT list_filter(inner_function(), '{\"a\": 1}' -> '$.a')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "list_filter"));
	REQUIRE(!pending_overload);
}
