#include "duckdb/common/vector/map_vector.hpp"
#include "core_functions/scalar/map_functions.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/list/contains_or_position.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
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

//! Whether the type has an explicit collation that changes how its values compare
static bool RequiresCollation(ClientContext &context, const LogicalType &type) {
	const auto has_collation = TypeVisitor::Contains(type, [](const LogicalType &child) {
		return child.id() == LogicalTypeId::VARCHAR && !StringType::GetCollation(child).empty();
	});
	if (!has_collation) {
		return false;
	}
	unique_ptr<Expression> collated_value = make_uniq<BoundReferenceExpression>(type, idx_t(0));
	return ExpressionBinder::PushCollation(context, collated_value, type, CollationType::COMBINABLE_COLLATIONS);
}

static unique_ptr<Expression> BindFunction(ClientContext &context, const Identifier &name,
                                           vector<unique_ptr<Expression>> children) {
	FunctionBinder function_binder(context);
	return function_binder.BindScalarFunction(Identifier::DefaultSchema(), name, std::move(children));
}

static unique_ptr<Expression> BindFunction(ClientContext &context, const Identifier &name, unique_ptr<Expression> first,
                                           unique_ptr<Expression> second = nullptr) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(first));
	if (second) {
		children.push_back(std::move(second));
	}
	return BindFunction(context, name, std::move(children));
}

//! The lookup of a collated key refers to the map three times, so invoke evaluates a volatile map once
static unique_ptr<Expression> BindVolatileMapLookup(FunctionBindExpressionInput &input) {
	const Identifier map_name("map");
	const Identifier key_name("key");
	// invoke passes its last argument as reference 0
	auto map_ref = make_uniq<BoundReferenceExpression>(map_name, input.children[0]->GetReturnType(), idx_t(1));
	auto key_ref = make_uniq<BoundReferenceExpression>(key_name, input.children[1]->GetReturnType(), idx_t(0));
	auto lookup = BindFunction(input.context, input.bound_function.GetName(), std::move(map_ref), std::move(key_ref));
	auto lambda =
	    make_uniq<BoundLambdaExpression>(ExpressionType::LAMBDA, LogicalType::LAMBDA, std::move(lookup), idx_t(2));
	lambda->SetParameterNames({map_name, key_name});
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(lambda));
	children.push_back(std::move(input.children[0]));
	children.push_back(std::move(input.children[1]));
	return BindFunction(input.context, "invoke", std::move(children));
}

//! A collated key is replaced by the first map key that list_position finds with the collation pushed into both
static unique_ptr<Expression> BindCollatedKey(FunctionBindExpressionInput &input) {
	auto &context = input.context;
	auto &map = input.children[0];
	auto &key = input.children[1];
	if (!RequiresCollation(context, MapType::KeyType(map->GetReturnType())) &&
	    !RequiresCollation(context, key->GetReturnType())) {
		return nullptr;
	}
	if (map->IsVolatile()) {
		return BindVolatileMapLookup(input);
	}
	auto map_keys = BindFunction(context, "map_keys", map->Copy());
	auto position = BindFunction(context, "list_position", map_keys->Copy(), std::move(key));
	key = BindFunction(context, "list_extract", std::move(map_keys), std::move(position));
	return nullptr;
}

ScalarFunction MapExtractValueFun::GetFunction() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");

	ScalarFunction fun({}, val_type, MapExtractValueFunc);
	fun.GetSignature().AddParameter("map", LogicalType::MAP(key_type, val_type)).AddParameter("key", key_type);
	fun.SetBindExpressionCallback(BindCollatedKey);
	// The collation of the key applies to the key search, not to the value
	fun.SetCollationHandling(FunctionCollationHandling::IGNORE_COLLATIONS);
	fun.SetStatisticsCallback(MapExtractValueStats);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

ScalarFunction MapExtractFun::GetFunction() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");

	ScalarFunction fun({}, LogicalType::LIST(val_type), MapExtractListFunc);
	fun.GetSignature().AddParameter("map", LogicalType::MAP(key_type, val_type)).AddParameter("key", key_type);
	fun.SetBindExpressionCallback(BindCollatedKey);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
