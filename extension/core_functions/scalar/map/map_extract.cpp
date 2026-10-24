#include "duckdb/common/vector/map_vector.hpp"
#include "core_functions/scalar/map_functions.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/list/contains_or_position.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"

namespace duckdb {

static void MapExtractValueFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto count = args.size();

	const auto &map_vec = args.data[0];
	const auto &arg_vec = args.data[1];

	const auto &key_vec = MapVector::GetKeys(map_vec);
	const auto &val_vec = MapVector::GetValues(map_vec);

	// Collect the matching positions
	Vector pos_vec(LogicalType::INTEGER, count);
	ListSearchOp<int32_t>(map_vec, key_vec, arg_vec, pos_vec, args.size());

	UnifiedVectorFormat pos_format;
	UnifiedVectorFormat lst_format;

	pos_vec.ToUnifiedFormat(pos_format);
	map_vec.ToUnifiedFormat(lst_format);

	const auto pos_data = UnifiedVectorFormat::GetData<int32_t>(pos_format);
	const auto inc_list_data = UnifiedVectorFormat::GetData<list_entry_t>(lst_format);

	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto lst_idx = lst_format.sel->get_index(row_idx);
		if (!lst_format.validity.RowIsValid(lst_idx)) {
			FlatVector::SetNull(result, row_idx, true);
			continue;
		}

		const auto pos_idx = pos_format.sel->get_index(row_idx);
		if (!pos_format.validity.RowIsValid(pos_idx)) {
			// We didnt find the key in the map, so return NULL
			FlatVector::SetNull(result, row_idx, true);
			continue;
		}

		// Compute the actual position of the value in the map value vector
		const auto pos = inc_list_data[lst_idx].offset + UnsafeNumericCast<idx_t>(pos_data[pos_idx] - 1);
		VectorOperations::Copy(val_vec, result, pos + 1, pos, row_idx);
	}

	if (args.size() == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}

	result.Verify();
}

static void MapExtractListFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto count = args.size();

	const auto &map_vec = args.data[0];
	const auto &arg_vec = args.data[1];

	const auto &key_vec = MapVector::GetKeys(map_vec);
	const auto &val_vec = MapVector::GetValues(map_vec);

	// Collect the matching positions
	Vector pos_vec(LogicalType::INTEGER, count);
	ListSearchOp<int32_t>(map_vec, key_vec, arg_vec, pos_vec, args.size());

	auto pos_entries = pos_vec.Values<int32_t>();
	auto map_entries = map_vec.Values<list_entry_t>();
	const auto val_size = ListVector::GetListSize(map_vec);
	auto out_list_data = FlatVector::Writer<list_entry_t>(result, count);

	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto map_entry = map_entries[row_idx];
		if (!map_entry.IsValid()) {
			out_list_data.WriteNull();
			continue;
		}

		auto list = out_list_data.WriteDynamicList();
		auto pos_entry = pos_entries[row_idx];
		if (!pos_entry.IsValid()) {
			// key not found: return empty list
			continue;
		}

		const auto &inc_list = map_entry.GetValue();
		const auto pos = inc_list.offset + UnsafeNumericCast<idx_t>(pos_entry.GetValue() - 1);
		SelectionVector sel(1);
		sel.set_index(0, pos);
		list.Append(val_vec, sel, val_size, 0, 1);
	}
}

static bool TypeHasCollation(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
		return !type.HasAlias() && !StringType::GetCollation(type).empty();
	case LogicalTypeId::LIST:
		return TypeHasCollation(ListType::GetChildType(type));
	case LogicalTypeId::ARRAY:
		return TypeHasCollation(ArrayType::GetChildType(type));
	default:
		return false;
	}
}

static unique_ptr<Expression> BindFunction(ClientContext &context, const char *name,
                                           vector<unique_ptr<Expression>> children) {
	FunctionBinder function_binder(context);
	return function_binder.BindScalarFunction(Identifier::DefaultSchema(), Identifier(name), std::move(children));
}

//! Binds invoke, which evaluates each argument once. The body refers to the last argument as reference 0
static unique_ptr<Expression> BindInvoke(ClientContext &context, unique_ptr<Expression> body,
                                         vector<unique_ptr<Expression>> arguments, vector<Identifier> names) {
	D_ASSERT(arguments.size() == names.size());
	auto lambda = make_uniq<BoundLambdaExpression>(ExpressionType::LAMBDA, LogicalType::LAMBDA, std::move(body),
	                                               arguments.size());
	lambda->SetParameterNames(std::move(names));
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(lambda));
	for (auto &argument : arguments) {
		children.push_back(std::move(argument));
	}
	return BindFunction(context, "invoke", std::move(children));
}

//! Whether evaluating the expression twice is cheap and gives the same result
static bool CanEvaluateTwice(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
	case ExpressionClass::BOUND_REF:
	case ExpressionClass::BOUND_LAMBDA_REF:
	case ExpressionClass::BOUND_CONSTANT:
	case ExpressionClass::BOUND_PARAMETER:
		return true;
	default:
		return false;
	}
}

//! Rebinds a lookup with a collated key as a lookup of the position in the keys, as list_position applies collations
static unique_ptr<Expression> BindCollatedMapExtract(FunctionBindExpressionInput &input, bool extract_list) {
	const auto &map_type = input.children[0]->GetReturnType();
	const auto &key_type = input.children[1]->GetReturnType();
	if (!TypeHasCollation(key_type) && !TypeHasCollation(MapType::KeyType(map_type))) {
		return nullptr;
	}
	auto &context = input.context;
	const Identifier map_name("map");
	const Identifier key_name("key");
	// The map is used twice, so other maps are evaluated once by invoke, which hides the statistics of their values.
	// Invoke also evaluates a volatile or throwing key when the map is NULL.
	const auto &key = *input.children[1];
	const bool use_invoke = !CanEvaluateTwice(*input.children[0]) || key.IsVolatile() || key.CanThrow();
	unique_ptr<Expression> map_expr;
	unique_ptr<Expression> key_expr;
	if (use_invoke) {
		map_expr = make_uniq<BoundReferenceExpression>(map_name, map_type, idx_t(1));
		key_expr = make_uniq<BoundReferenceExpression>(key_name, key_type, idx_t(0));
	} else {
		map_expr = std::move(input.children[0]);
		key_expr = std::move(input.children[1]);
	}

	vector<unique_ptr<Expression>> children;
	children.push_back(map_expr->Copy());
	auto keys = BindFunction(context, "map_keys", std::move(children));
	children.clear();
	children.push_back(std::move(keys));
	children.push_back(std::move(key_expr));
	auto position = BindFunction(context, "list_position", std::move(children));
	children.clear();
	children.push_back(std::move(map_expr));
	auto values = BindFunction(context, "map_values", std::move(children));

	unique_ptr<Expression> body;
	if (extract_list) {
		// Select the value at the position, or no value for a missing key
		const Identifier values_name("values");
		const Identifier position_name("position");
		const auto &position_type = position->GetReturnType();
		auto position_ref = make_uniq<BoundReferenceExpression>(position_name, position_type, idx_t(0));
		auto is_missing = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
		is_missing->GetChildrenMutable().push_back(position_ref->Copy());
		children.clear();
		children.push_back(std::move(position_ref));
		auto single_index = BindFunction(context, "list_value", std::move(children));
		auto no_index = make_uniq<BoundConstantExpression>(Value::LIST(position_type, vector<Value>()));
		auto indexes =
		    make_uniq<BoundCaseExpression>(std::move(is_missing), std::move(no_index), std::move(single_index));
		children.clear();
		children.push_back(make_uniq<BoundReferenceExpression>(values_name, values->GetReturnType(), idx_t(1)));
		children.push_back(std::move(indexes));
		auto selection = BindFunction(context, "list_select", std::move(children));

		vector<unique_ptr<Expression>> arguments;
		arguments.push_back(std::move(values));
		arguments.push_back(std::move(position));
		body = BindInvoke(context, std::move(selection), std::move(arguments), {values_name, position_name});
	} else {
		children.clear();
		children.push_back(std::move(values));
		children.push_back(std::move(position));
		body = BindFunction(context, "list_extract", std::move(children));
	}

	if (use_invoke) {
		vector<unique_ptr<Expression>> arguments;
		arguments.push_back(std::move(input.children[0]));
		arguments.push_back(std::move(input.children[1]));
		body = BindInvoke(context, std::move(body), std::move(arguments), {map_name, key_name});
	}
	return BoundCastExpression::AddCastToType(context, std::move(body), input.bound_function.GetReturnType());
}

static unique_ptr<Expression> BindMapExtractValueExpression(FunctionBindExpressionInput &input) {
	return BindCollatedMapExtract(input, false);
}

static unique_ptr<Expression> BindMapExtractExpression(FunctionBindExpressionInput &input) {
	return BindCollatedMapExtract(input, true);
}

static unique_ptr<BaseStatistics> MapExtractValueStats(ClientContext &, FunctionStatisticsInput &input) {
	auto &entry_stats = ListStats::GetChildStats(input.child_stats[0]);
	auto value_copy = StructStats::GetChildStats(entry_stats, 1).Copy();
	// missing keys return NULL
	value_copy.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	if (!input.child_stats[0].CanHaveNoNull() || !input.child_stats[1].CanHaveNoNull()) {
		value_copy.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
	}
	return value_copy.ToUnique();
}

ScalarFunction MapExtractValueFun::GetFunction() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");

	ScalarFunction fun({}, val_type, MapExtractValueFunc);
	fun.GetSignature().AddParameter("map", LogicalType::MAP(key_type, val_type)).AddParameter("key", key_type);
	fun.SetStatisticsCallback(MapExtractValueStats);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetBindExpressionCallback(BindMapExtractValueExpression);
	return fun;
}

ScalarFunction MapExtractFun::GetFunction() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");

	ScalarFunction fun({}, LogicalType::LIST(val_type), MapExtractListFunc);
	fun.GetSignature().AddParameter("map", LogicalType::MAP(key_type, val_type)).AddParameter("key", key_type);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetBindExpressionCallback(BindMapExtractExpression);
	return fun;
}

} // namespace duckdb
