#include "duckdb/common/vector/map_vector.hpp"
#include "core_functions/scalar/map_functions.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/list/contains_or_position.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"

namespace duckdb {

struct MapExtractCollationData final : public FunctionData {
	explicit MapExtractCollationData(unique_ptr<Expression> key_position_p) : key_position(std::move(key_position_p)) {
	}

	//! The position of the key in the map keys, with the collation pushed into both
	unique_ptr<Expression> key_position;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MapExtractCollationData>(key_position->Copy());
	}

	bool Equals(const FunctionData &other_p) const override {
		return key_position->Equals(*other_p.Cast<MapExtractCollationData>().key_position);
	}
};

struct MapExtractCollationState final : public FunctionLocalState {
	static unique_ptr<FunctionLocalState> Init(ExpressionState &, const BoundFunctionExpression &,
	                                           FunctionData *bind_data) {
		return bind_data ? make_uniq<MapExtractCollationState>() : nullptr;
	}

	//! Created on first use, as the executor needs a client context that binding an index does not have
	unique_ptr<ExpressionExecutor> executor;
};

//! Finds the position of the key in each map, comparing collated values when the lookup has a collation
static void FindKeyPositions(DataChunk &args, ExpressionState &state, Vector &positions) {
	auto local_state = ExecuteFunctionState::GetFunctionState(state);
	if (!local_state) {
		const auto &map_vec = args.data[0];
		ListSearchOp<int32_t>(map_vec, MapVector::GetKeys(map_vec), args.data[1], positions, args.size());
		return;
	}
	auto &executor = local_state->Cast<MapExtractCollationState>().executor;
	if (!executor) {
		auto &bind_data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<MapExtractCollationData>();
		executor = make_uniq<ExpressionExecutor>(state.GetContext(), *bind_data.key_position);
	}
	executor->ExecuteExpression(args, positions);
}

static void MapExtractValueFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto count = args.size();

	const auto &map_vec = args.data[0];
	const auto &val_vec = MapVector::GetValues(map_vec);

	// Collect the matching positions
	Vector pos_vec(LogicalType::INTEGER, count);
	FindKeyPositions(args, state, pos_vec);

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
	const auto &val_vec = MapVector::GetValues(map_vec);

	// Collect the matching positions
	Vector pos_vec(LogicalType::INTEGER, count);
	FindKeyPositions(args, state, pos_vec);

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

//! A lookup with a collated key or map keys searches with list_position, which pushes the collation into both
static unique_ptr<FunctionData> MapExtractBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	const auto &map_type = arguments[0]->GetReturnType();
	const auto &key_type = arguments[1]->GetReturnType();
	if (!RequiresCollation(context, MapType::KeyType(map_type)) && !RequiresCollation(context, key_type)) {
		return nullptr;
	}
	FunctionBinder function_binder(context);
	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(map_type, idx_t(0)));
	auto keys =
	    function_binder.BindScalarFunction(Identifier::DefaultSchema(), Identifier("map_keys"), std::move(children));
	children.clear();
	children.push_back(std::move(keys));
	children.push_back(make_uniq<BoundReferenceExpression>(key_type, idx_t(1)));
	auto key_position = function_binder.BindScalarFunction(Identifier::DefaultSchema(), Identifier("list_position"),
	                                                       std::move(children));
	return make_uniq<MapExtractCollationData>(std::move(key_position));
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
	fun.SetBindCallback(MapExtractBind);
	fun.SetInitStateCallback(MapExtractCollationState::Init);
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
	fun.SetBindCallback(MapExtractBind);
	fun.SetInitStateCallback(MapExtractCollationState::Init);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
