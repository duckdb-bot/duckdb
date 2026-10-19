#include "catch.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension/linked_extension_registry.hpp"
#include "duckdb/main/extension_helper.hpp"
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

unique_ptr<ScalarFunction> EchoOverload(const string &name, vector<LogicalType> arguments = {LogicalType::VARCHAR}) {
	return make_uniq<ScalarFunction>(Identifier(name), std::move(arguments), LogicalType::VARCHAR, EchoFunction);
}

//! Registers outer_function(INTEGER), and inner_function() whose bind adds the pending overload
void RegisterTestFunctions(DuckDB &db) {
	RegisterFunction(*db.instance,
	                 ScalarFunction("outer_function", {LogicalType::INTEGER}, LogicalType::INTEGER, AddOneFunction));
	ScalarFunction inner("inner_function", {}, LogicalType::VARCHAR, DuckFunction);
	inner.SetBindCallback(RegisterOverloadBind);
	RegisterFunction(*db.instance, std::move(inner));
}

//! Opens a database that autoloads the extensions linked into this binary instead of loading them
unique_ptr<DuckDB> OpenWithoutLoadedExtensions() {
	DBConfig config;
	config.options.load_extensions = false;
	config.SetOptionByName("autoload_known_extensions", true);
	config.SetOptionByName("autoinstall_known_extensions", false);
	return make_uniq<DuckDB>(nullptr, &config);
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
	if (!db.ExtensionIsLoaded("json")) {
		SUCCEED("json is not linked into this binary, so the arrow cannot be bound as a JSON operator");
		return;
	}
	RegisterTestFunctions(db);
	Connection con(db);

	// an arrow that does not bind as a lambda is bound again as a JSON operator, with the overload added meanwhile
	pending_overload = EchoOverload("list_filter", {LogicalType::VARCHAR, LogicalType::ANY});
	auto result = con.Query("SELECT list_filter(inner_function(), '{\"a\": 1}' -> '$.a')");
	REQUIRE(CHECK_COLUMN(result, 0, {"duck"}));
	REQUIRE(!pending_overload);
}

TEST_CASE("Test autoloading an extension while a function call is bound", "[api]") {
	bool icu_linked = false;
	for (auto &linked : LinkedExtensionRegistry::Get()) {
		icu_linked = icu_linked || linked.name == "icu";
	}
	if (!icu_linked || !ExtensionHelper::CanAutoloadExtension(*OpenWithoutLoadedExtensions()->instance, "icu")) {
		SUCCEED("icu is not linked into this binary or cannot be autoloaded");
		return;
	}
	struct AutoloadCase {
		string query;
		Value expected;
	};
	vector<AutoloadCase> cases {
	    // current_date loads icu while the arithmetic around it is bound, in the select list and in a WHERE clause
	    {"SELECT (current_date + 1) - current_date", Value::BIGINT(1)},
	    {"SELECT 42 WHERE current_date - 1 < current_date", Value::INTEGER(42)},
	    // date_part of a TIMESTAMPTZ loads icu from its bind callback
	    {"SELECT date_part('year', now()) + 1 > 2000", Value::BOOLEAN(true)},
	    // icu adds overloads of the table function range
	    {"SELECT count(*) FROM range(current_date, current_date + 3, INTERVAL 1 DAY)", Value::BIGINT(3)},
	    {"SELECT 1 + (SELECT current_date - current_date)", Value::BIGINT(1)},
	    {"SELECT list_transform([1, 2], lambda x: (current_date + x) - current_date)::VARCHAR", Value("[1, 2]")},
	    // the call is bound with the overload of + that icu adds
	    {"SELECT typeof(current_date::TIMESTAMPTZ + INTERVAL 1 DAY)", Value("TIMESTAMP WITH TIME ZONE")},
	};
	for (auto &autoload_case : cases) {
		auto db = OpenWithoutLoadedExtensions();
		Connection con(*db);
		REQUIRE(!db->ExtensionIsLoaded("icu"));
		auto result = con.Query(autoload_case.query);
		REQUIRE(CHECK_COLUMN(result, 0, {autoload_case.expected}));
		REQUIRE(db->ExtensionIsLoaded("icu"));
	}

	// a call without a matching overload is still an error
	auto db = OpenWithoutLoadedExtensions();
	Connection con(*db);
	auto result = con.Query("SELECT current_date + [1]");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "No function matches"));
}
