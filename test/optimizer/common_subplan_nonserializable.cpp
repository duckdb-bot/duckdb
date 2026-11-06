#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

struct NonSerializableGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
	int32_t value = 42;
};

struct NonSerializableBindData : public TableFunctionData {
	unique_ptr<FunctionData> Copy() const override {
		throw InternalException("Copy not supported for NonSerializableBindData");
	}

	bool Equals(const FunctionData &other) const override {
		return false;
	}

	bool SupportStatementCache() const override {
		return false;
	}
};

unique_ptr<FunctionData> NonSerializableBind(ClientContext &, TableFunctionBindInput &,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("i");
	return make_uniq<NonSerializableBindData>();
}

unique_ptr<GlobalTableFunctionState> NonSerializableInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<NonSerializableGlobalState>();
}

void NonSerializableFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<NonSerializableGlobalState>();
	if (state.emitted) {
		output.SetChildCardinality(0);
		return;
	}
	auto result_data = FlatVector::GetDataMutable<int32_t>(output.data[0]);
	result_data[0] = state.value;
	output.SetChildCardinality(1);
	state.emitted = true;
}

void RegisterNonSerializableTableFunction(DuckDB &db) {
	// stable under the default declaration - sharing is blocked by the missing serialization callbacks alone
	TableFunction table_function("nonserial_tf", {}, NonSerializableFunction, NonSerializableBind, NonSerializableInit);

	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(table_function);
}

// x from 0 to n - 1, keeping the pushed filters "x < c" and "x >= c" in the bind data
struct BoundedRangeBindData : public TableFunctionData {
	int64_t n = 0;
	int64_t lower = NumericLimits<int64_t>::Minimum();
	int64_t upper = NumericLimits<int64_t>::Maximum();

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<BoundedRangeBindData>();
		result->n = n;
		result->lower = lower;
		result->upper = upper;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<BoundedRangeBindData>();
		return n == other.n && lower == other.lower && upper == other.upper;
	}
};

struct BoundedRangeGlobalState : public GlobalTableFunctionState {
	int64_t position = 0;
};

unique_ptr<FunctionData> BoundedRangeBind(ClientContext &, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("x");
	auto result = make_uniq<BoundedRangeBindData>();
	result->n = input.inputs[0].GetValue<int64_t>();
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> BoundedRangeInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<BoundedRangeGlobalState>();
}

void BoundedRangeFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<BoundedRangeBindData>();
	auto &state = data.global_state->Cast<BoundedRangeGlobalState>();
	const auto start = MaxValue(state.position, bind_data.lower);
	const auto end = MinValue(bind_data.n, bind_data.upper);
	const idx_t count = start < end ? MinValue<idx_t>(NumericCast<idx_t>(end - start), STANDARD_VECTOR_SIZE) : 0;
	auto writer = FlatVector::Writer<int64_t>(output.data[0], count);
	for (idx_t i = 0; i < count; i++) {
		writer.WriteValue(start + NumericCast<int64_t>(i));
	}
	state.position = start + NumericCast<int64_t>(count);
	output.SetChildCardinality(count);
}

void BoundedRangePushdown(ClientContext &, LogicalGet &, FunctionData *bind_data_p,
                          vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<BoundedRangeBindData>();
	for (idx_t i = 0; i < filters.size(); i++) {
		auto type = filters[i]->GetExpressionType();
		if (filters[i]->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION ||
		    (type != ExpressionType::COMPARE_LESSTHAN && type != ExpressionType::COMPARE_GREATERTHANOREQUALTO)) {
			continue;
		}
		auto &comparison = filters[i]->Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		if (left.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
		    right.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			continue;
		}
		auto constant = right.Cast<BoundConstantExpression>().GetValue().GetValue<int64_t>();
		if (type == ExpressionType::COMPARE_LESSTHAN) {
			bind_data.upper = constant;
		} else {
			bind_data.lower = constant;
		}
		filters.erase_at(i);
		i--;
	}
}

void RegisterBoundedRangeTableFunction(DuckDB &db) {
	TableFunction table_function("bounded_range", {LogicalType::BIGINT}, BoundedRangeFunction, BoundedRangeBind,
	                             BoundedRangeInit);
	table_function.pushdown_complex_filter = BoundedRangePushdown;

	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(table_function);
}

atomic<int32_t> non_repeatable_scan_count {0};

unique_ptr<GlobalTableFunctionState> NonRepeatableInit(ClientContext &, TableFunctionInitInput &) {
	auto result = make_uniq<NonSerializableGlobalState>();
	result->value = ++non_repeatable_scan_count;
	return std::move(result);
}

unique_ptr<FunctionData> RepeatabilityBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("i");
	// Rebinding these parameterized calls is supported, independently of whether their scans are repeatable.
	return make_uniq<TableFunctionData>();
}

void RegisterRepeatabilityTableFunctions(DuckDB &db) {
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	TableFunction unknown("repeatability_unknown", {LogicalType::INTEGER}, NonSerializableFunction, RepeatabilityBind,
	                      NonRepeatableInit);
	loader.RegisterFunction(unknown);
	TableFunction non_repeatable("repeatability_false", {LogicalType::INTEGER}, NonSerializableFunction,
	                             RepeatabilityBind, NonRepeatableInit);
	non_repeatable.stability = FunctionStability::VOLATILE;
	loader.RegisterFunction(non_repeatable);
	TableFunction repeatable("repeatability_true", {LogicalType::INTEGER}, NonSerializableFunction, RepeatabilityBind,
	                         NonSerializableInit);
	repeatable.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	loader.RegisterFunction(repeatable);
}

atomic<int32_t> zero_argument_bind_count {0};

struct ZeroArgumentBindData : public TableFunctionData {
	// Statement caching stays supported: rebinding this call is fine, but each bind captures its own row.
	int32_t bound_value = 0;
};

unique_ptr<FunctionData> ZeroArgumentBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                          vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("i");
	auto result = make_uniq<ZeroArgumentBindData>();
	result->bound_value = ++zero_argument_bind_count;
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> ZeroArgumentInit(ClientContext &, TableFunctionInitInput &input) {
	auto result = make_uniq<NonSerializableGlobalState>();
	result->value = input.bind_data->Cast<ZeroArgumentBindData>().bound_value;
	return std::move(result);
}

void RegisterZeroArgumentTableFunction(DuckDB &db) {
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	// stable under the default declaration - sharing is blocked by the zero-positional-argument identity guard
	TableFunction zero_argument("repeatability_zero_arg", {}, NonSerializableFunction, ZeroArgumentBind,
	                            ZeroArgumentInit);
	loader.RegisterFunction(zero_argument);
}

string GetExplainPlan(Connection &con, const string &query) {
	auto result = con.Query("EXPLAIN " + query);
	REQUIRE_NO_FAIL(*result);

	string explain;
	for (idx_t row = 0; row < result->RowCount(); row++) {
		for (idx_t col = 0; col < result->ColumnCount(); col++) {
			explain += result->Collection().GetValue(col, row).ToString();
			explain += "\n";
		}
	}
	return explain;
}

} // namespace

TEST_CASE("Common subplan skips table functions without serialization callbacks", "[optimizer][common_subplan]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterNonSerializableTableFunction(db);

	const string query = "SELECT lhs.c, rhs.c "
	                     "FROM (SELECT count(*) AS c FROM nonserial_tf()) lhs "
	                     "CROSS JOIN (SELECT count(*) AS c FROM nonserial_tf()) rhs";

	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 1);
	REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == 1);
	REQUIRE(result->Collection().GetValue(1, 0).GetValue<int64_t>() == 1);

	REQUIRE_NO_FAIL(con.Query("PRAGMA explain_output='optimized_only'"));
	const auto explain = GetExplainPlan(con, query);
	REQUIRE(!StringUtil::Contains(explain, "__common_subplan_"));
}

TEST_CASE("Common subplan skips table functions that keep pushed filters in their bind data",
          "[optimizer][common_subplan]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterBoundedRangeTableFunction(db);
	// the scalar aggregate merger would compute the sibling aggregates in one scan before the filter pushdown
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers = 'scalar_aggregate_merge'"));

	const string query = "SELECT (SELECT count(*) FROM bounded_range(10) WHERE x < 3) AS below, "
	                     "(SELECT count(*) FROM bounded_range(10) WHERE x >= 3) AS above";

	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 1);
	REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == 3);
	REQUIRE(result->Collection().GetValue(1, 0).GetValue<int64_t>() == 7);

	REQUIRE_NO_FAIL(con.Query("PRAGMA explain_output='optimized_only'"));
	const auto explain = GetExplainPlan(con, query);
	REQUIRE(!StringUtil::Contains(explain, "__common_subplan_"));

	// a filter that is not consumed stays in the plan, so scans with it are still shared
	const string kept_filter_query = "SELECT (SELECT count(*) FROM bounded_range(10) WHERE x % 2 = 0) AS lhs, "
	                                 "(SELECT count(*) FROM bounded_range(10) WHERE x % 2 = 0) AS rhs";
	REQUIRE(StringUtil::Contains(GetExplainPlan(con, kept_filter_query), "__common_subplan_"));

	// scans that consumed part of their filters are kept apart
	const string partial_query = "SELECT (SELECT count(*) FROM bounded_range(10) WHERE x < 3 AND x % 2 = 0) AS lhs, "
	                             "(SELECT count(*) FROM bounded_range(10) WHERE x < 5 AND x % 2 = 0) AS rhs";
	result = con.Query(partial_query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == 2);
	REQUIRE(result->Collection().GetValue(1, 0).GetValue<int64_t>() == 3);

	// scans that kept no pushed filters are still shared
	const string unfiltered_query = "SELECT (SELECT count(*) FROM bounded_range(10)) AS lhs, "
	                                "(SELECT count(*) FROM bounded_range(10)) AS rhs";
	REQUIRE(StringUtil::Contains(GetExplainPlan(con, unfiltered_query), "__common_subplan_"));
	const string repeat_query = "SELECT (SELECT count(*) FROM repeat(42, 10)) AS lhs, "
	                            "(SELECT count(*) FROM repeat(42, 10)) AS rhs";
	REQUIRE(StringUtil::Contains(GetExplainPlan(con, repeat_query), "__common_subplan_"));
}

// SQL cannot register the stability declaration or make an opaque scan return a different value
// deterministically on each initialization. Keep that API contract here, with builtin scan coverage in SQL.
TEST_CASE("Common subplans follow the declared table function stability", "[optimizer][common_subplan]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterRepeatabilityTableFunctions(db);
	REQUIRE_NO_FAIL(con.Query("SET explain_output='optimized_only'"));

	// an explicitly volatile scan is never shared
	const string volatile_query = "SELECT count(*) FROM (SELECT i FROM repeatability_false(7) "
	                              "INTERSECT ALL SELECT i FROM repeatability_false(7))";
	non_repeatable_scan_count = 0;
	auto result = con.Query(volatile_query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE(non_repeatable_scan_count.load() == 2);
	REQUIRE(!StringUtil::Contains(GetExplainPlan(con, volatile_query), "__common_subplan_"));

	// contract decision from the PR review: an undeclared scan is stable within the query and shares like an
	// explicitly declared one. This scan violates the default contract on purpose - a scan whose rows change
	// per initialization must declare VOLATILE
	const string unknown_query = "SELECT count(*) FROM (SELECT i FROM repeatability_unknown(7) "
	                             "INTERSECT ALL SELECT i FROM repeatability_unknown(7))";
	non_repeatable_scan_count = 0;
	result = con.Query(unknown_query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(non_repeatable_scan_count.load() == 1);
	REQUIRE(StringUtil::Contains(GetExplainPlan(con, unknown_query), "__common_subplan_"));

	// Positional parameters identify this stable scan without serializing its bind data.
	const string repeatable_query = "SELECT count(*) FROM (SELECT i FROM repeatability_true(7) "
	                                "INTERSECT ALL SELECT i FROM repeatability_true(7))";
	result = con.Query(repeatable_query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(StringUtil::Contains(GetExplainPlan(con, repeatable_query), "__common_subplan_"));
}

// SQL cannot register a statement-cache-supporting, default-stable scan whose separate binds capture
// different fixed rows. Keep the zero-positional-argument identity guard contract here.
TEST_CASE("Common subplans require positional parameters to identify repeatable scans", "[optimizer][common_subplan]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterZeroArgumentTableFunction(db);
	REQUIRE_NO_FAIL(con.Query("SET explain_output='optimized_only'"));

	// Without serialization callbacks or positional parameters, equal serialized bytes cannot prove that
	// two binds captured the same rows, so the identity guard must keep sharing disabled.
	const string query = "SELECT count(*) FROM (SELECT i FROM repeatability_zero_arg() "
	                     "INTERSECT ALL SELECT i FROM repeatability_zero_arg())";
	zero_argument_bind_count = 0;
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE(zero_argument_bind_count.load() >= 2);
	REQUIRE(!StringUtil::Contains(GetExplainPlan(con, query), "__common_subplan_"));
}
