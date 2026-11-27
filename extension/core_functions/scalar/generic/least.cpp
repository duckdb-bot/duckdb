#include "duckdb/common/types/decimal.hpp"
#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

namespace duckdb {

namespace {
struct LeastOp {
	using OP = LessThan;

	static OrderByNullType NullOrdering() {
		return OrderByNullType::NULLS_LAST;
	}
};

struct GreaterOp {
	using OP = GreaterThan;

	static OrderByNullType NullOrdering() {
		return OrderByNullType::NULLS_FIRST;
	}
};

template <class OP>
struct LeastOperator {
	template <class T>
	static T Operation(T left, T right) {
		return OP::Operation(left, right) ? left : right;
	}
};

struct LeastGreatestSortKeyState : public FunctionLocalState {
	explicit LeastGreatestSortKeyState(idx_t column_count, OrderByNullType null_ordering)
	    : intermediate(LogicalType::BLOB), modifiers(OrderType::ASCENDING, null_ordering) {
		vector<LogicalType> types;
		// initialize sort key chunk
		for (idx_t i = 0; i < column_count; i++) {
			types.push_back(LogicalType::BLOB);
		}
		sort_keys.Initialize(Allocator::DefaultAllocator(), types);
	}

	DataChunk sort_keys;
	Vector intermediate;
	OrderModifiers modifiers;
};

template <class OP>
unique_ptr<FunctionLocalState> LeastGreatestSortKeyInit(ExpressionState &state, const BoundFunctionExpression &expr,
                                                        optional_ptr<const FunctionData> bind_data) {
	return make_uniq<LeastGreatestSortKeyState>(expr.GetChildren().size(), OP::NullOrdering());
}

template <bool STRING>
struct StandardLeastGreatest {
	static constexpr bool IS_STRING = STRING;

	static DataChunk &Prepare(DataChunk &args, ExpressionState &) {
		return args;
	}

	static Vector &TargetVector(Vector &result, ExpressionState &) {
		return result;
	}

	static void FinalizeResult(idx_t rows, bool result_has_value[], Vector &result, ExpressionState &) {
		auto &result_mask = FlatVector::ValidityMutable(result);
		for (idx_t i = 0; i < rows; i++) {
			if (!result_has_value[i]) {
				result_mask.SetInvalid(i);
			}
		}
	}
};

struct SortKeyLeastGreatest {
	static constexpr bool IS_STRING = false;

	static DataChunk &Prepare(DataChunk &args, ExpressionState &state) {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LeastGreatestSortKeyState>();
		lstate.sort_keys.Reset();
		for (idx_t c_idx = 0; c_idx < args.ColumnCount(); c_idx++) {
			CreateSortKeyHelpers::CreateSortKey(args.data[c_idx], lstate.modifiers, lstate.sort_keys.data[c_idx]);
		}
		return lstate.sort_keys;
	}

	static Vector &TargetVector(Vector &result, ExpressionState &state) {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LeastGreatestSortKeyState>();
		return lstate.intermediate;
	}

	static void FinalizeResult(idx_t rows, bool result_has_value[], Vector &result, ExpressionState &state) {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<LeastGreatestSortKeyState>();
		auto result_keys = FlatVector::GetData<string_t>(lstate.intermediate);
		auto &result_mask = FlatVector::ValidityMutable(result);
		for (idx_t i = 0; i < rows; i++) {
			if (!result_has_value[i]) {
				result_mask.SetInvalid(i);
			} else {
				CreateSortKeyHelpers::DecodeSortKey(result_keys[i], result, i, lstate.modifiers);
			}
		}
	}
};

template <class T, class OP, class BASE_OP = StandardLeastGreatest<false>>
void LeastGreatestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	if (args.ColumnCount() == 1) {
		// single input: nop
		result.Reference(args.data[0]);
		return;
	}
	auto &input = BASE_OP::Prepare(args, state);
	auto &result_vector = BASE_OP::TargetVector(result, state);

	auto result_type = VectorType::CONSTANT_VECTOR;
	for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
		if (args.data[col_idx].GetVectorType() != VectorType::CONSTANT_VECTOR) {
			// non-constant input: result is not a constant vector
			result_type = VectorType::FLAT_VECTOR;
		}
		if (BASE_OP::IS_STRING) {
			// for string vectors we add a reference to the heap of the children
			StringVector::AddHeapReference(result_vector, input.data[col_idx]);
		}
	}

	auto result_data = FlatVector::ScatterWriter<T>(result_vector);
	bool result_has_value[STANDARD_VECTOR_SIZE] {false};
	// perform the operation column-by-column
	for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
		if (input.data[col_idx].GetVectorType() == VectorType::CONSTANT_VECTOR &&
		    ConstantVector::IsNull(input.data[col_idx])) {
			// ignore null vector
			continue;
		}

		auto entries = input.data[col_idx].template Values<T>();

		if (entries.CanHaveNull()) {
			// potential new null entries: have to check the null mask
			for (idx_t i = 0; i < input.size(); i++) {
				auto entry = entries[i];
				if (entry.IsValid()) {
					// not a null entry: perform the operation and add to new set
					auto ivalue = entry.GetValue();
					if (!result_has_value[i] || OP::template Operation<T>(ivalue, result_data[i])) {
						result_has_value[i] = true;
						result_data[i] = ivalue;
					}
				}
			}
		} else {
			// no new null entries: only need to perform the operation
			for (idx_t i = 0; i < input.size(); i++) {
				auto ivalue = entries.GetValueUnsafe(i);
				if (!result_has_value[i] || OP::template Operation<T>(ivalue, result_data[i])) {
					result_has_value[i] = true;
					result_data[i] = ivalue;
				}
			}
		}
	}
	BASE_OP::FinalizeResult(input.size(), result_has_value, result, state);
	result.SetVectorType(result_type);
}

//! The key that least and greatest compare their arguments by, for a type that has a collation
struct CollatedLeastGreatestData : public FunctionData {
	explicit CollatedLeastGreatestData(unique_ptr<Expression> key_p) : key(std::move(key_p)) {
	}

	//! The collation key of an argument, over a reference to column 0
	unique_ptr<Expression> key;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CollatedLeastGreatestData>(key->Copy());
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CollatedLeastGreatestData>();
		return Expression::Equals(*key, *other.key);
	}
};

struct CollatedLeastGreatestState : public LeastGreatestSortKeyState {
	CollatedLeastGreatestState(Context &context, const Expression &key, const LogicalType &argument_type,
	                           idx_t column_count, OrderByNullType null_ordering)
	    : LeastGreatestSortKeyState(column_count, null_ordering), argument_rows(STANDARD_VECTOR_SIZE) {
		for (idx_t i = 0; i < column_count; i++) {
			executors.push_back(make_uniq<ExpressionExecutor>(context, key));
		}
		argument.InitializeEmpty({argument_type});
		vector<LogicalType> key_types(column_count, key.GetReturnType());
		keys.Initialize(Allocator::DefaultAllocator(), key_types);
	}

	//! Compute the keys, one executor per argument, as a key can reference the buffers of its executor
	vector<unique_ptr<ExpressionExecutor>> executors;
	//! The argument that the key is computed of
	DataChunk argument;
	//! The keys of the arguments
	DataChunk keys;
	//! The rows that take their result from one argument
	SelectionVector argument_rows;
};

template <class OP>
unique_ptr<FunctionLocalState> CollatedLeastGreatestInit(ExpressionState &state, const BoundFunctionExpression &expr,
                                                         optional_ptr<const FunctionData> bind_data) {
	auto &key = *bind_data->Cast<CollatedLeastGreatestData>().key;
	return make_uniq<CollatedLeastGreatestState>(state.GetContext(), key, expr.GetReturnType(),
	                                             expr.GetChildren().size(), OP::NullOrdering());
}

//! Compares the arguments by their keys and returns the argument itself, as arguments that differ can share a key
template <class T, class OP, class BASE_OP = StandardLeastGreatest<false>>
void CollatedLeastGreatestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<CollatedLeastGreatestState>();
	const auto count = args.size();
	lstate.keys.Reset();
	for (idx_t col_idx = 0; col_idx < args.ColumnCount(); col_idx++) {
		lstate.argument.Reset();
		lstate.argument.data[0].Reference(args.data[col_idx]);
		lstate.argument.CheckCardinality(count);
		lstate.executors[col_idx]->ExecuteExpression(lstate.argument, lstate.keys.data[col_idx]);
	}
	lstate.keys.CheckCardinality(count);
	auto &input = BASE_OP::Prepare(lstate.keys, state);

	// the argument with the least or greatest key of each row, a NULL argument is skipped
	T winning_keys[STANDARD_VECTOR_SIZE];
	idx_t winners[STANDARD_VECTOR_SIZE];
	bool has_winner[STANDARD_VECTOR_SIZE] {false};
	for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
		auto entries = input.data[col_idx].template Values<T>();
		for (idx_t i = 0; i < count; i++) {
			auto entry = entries[i];
			if (!entry.IsValid()) {
				continue;
			}
			auto key = entry.GetValue();
			if (!has_winner[i] || OP::template Operation<T>(key, winning_keys[i])) {
				has_winner[i] = true;
				winning_keys[i] = key;
				winners[i] = col_idx;
			}
		}
	}

	// gather the rows of each argument behind each other, the result reads them in the order of the rows
	Vector gathered(result.GetType());
	SelectionVector result_rows(count);
	idx_t gathered_count = 0;
	for (idx_t col_idx = 0; col_idx < args.ColumnCount(); col_idx++) {
		idx_t row_count = 0;
		for (idx_t i = 0; i < count; i++) {
			if (has_winner[i] && winners[i] == col_idx) {
				lstate.argument_rows.set_index(row_count, i);
				result_rows.set_index(i, gathered_count + row_count);
				row_count++;
			}
		}
		if (row_count == count) {
			result.Reference(args.data[col_idx]);
			return;
		}
		if (row_count == 0) {
			continue;
		}
		VectorOperations::Copy(args.data[col_idx], gathered, lstate.argument_rows, row_count, 0, gathered_count);
		gathered_count += row_count;
	}
	if (gathered_count < count) {
		// the rows whose arguments are all NULL read a NULL behind the gathered rows
		FlatVector::SetNull(gathered, gathered_count, true);
		for (idx_t i = 0; i < count; i++) {
			if (!has_winner[i]) {
				result_rows.set_index(i, gathered_count);
			}
		}
	}
	result.Slice(gathered, result_rows, count);
}

template <class LEAST_GREATER_OP>
unique_ptr<BaseStatistics> PropagateLeastGreatestStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	auto &return_type = expr.GetReturnType();
	if (BaseStatistics::GetStatsType(return_type) != StatisticsType::NUMERIC_STATS ||
	    return_type.InternalType() == PhysicalType::BOOL) {
		return nullptr;
	}
	constexpr bool IS_LEAST = std::is_same_v<LEAST_GREATER_OP, LeastOp>;

	auto merge = [](Value &bound, const Value &candidate, bool keep_smaller) {
		if (bound.IsNull() || (keep_smaller ? candidate < bound : candidate > bound)) {
			bound = candidate;
		}
	};

	// least/greatest always returns one of its inputs, so the result range is built from the
	// inputs' min/max values. A NULL input is skipped at runtime, so only inputs that can never
	// be NULL may tighten the "anchored" side of the range:
	//
	//             | loose side (over all inputs) | anchored side (over non-null inputs)
	//   least     | min of the mins  -> min      | min of the maxes -> max
	//   greatest  | max of the maxes -> max      | max of the mins  -> min
	//
	// If every input can be NULL, a single input may end up deciding the result, so the anchored
	// side falls back to the opposite aggregation over all inputs
	Value loose;
	Value anchored;          // over non-null inputs only
	Value anchored_fallback; // over all inputs; used only when every input is nullable
	bool has_nonnull_input = false;
	for (auto &cs : child_stats) {
		if (cs.GetStatsType() != StatisticsType::NUMERIC_STATS || !NumericStats::HasMinMax(cs)) {
			return nullptr;
		}
		const Value cmin = NumericStats::Min(cs);
		const Value cmax = NumericStats::Max(cs);
		const Value &loose_val = IS_LEAST ? cmin : cmax;
		const Value &anchored_val = IS_LEAST ? cmax : cmin;

		merge(loose, loose_val, /*keep_smaller=*/IS_LEAST);
		merge(anchored_fallback, anchored_val, /*keep_smaller=*/!IS_LEAST);
		if (!cs.CanHaveNull()) {
			has_nonnull_input = true;
			merge(anchored, anchored_val, /*keep_smaller=*/IS_LEAST);
		}
	}
	if (!has_nonnull_input) {
		anchored = std::move(anchored_fallback);
	}

	auto result = NumericStats::CreateEmpty(return_type);
	if constexpr (IS_LEAST) {
		NumericStats::SetMin(result, loose);
		NumericStats::SetMax(result, anchored);
	} else {
		NumericStats::SetMin(result, anchored);
		NumericStats::SetMax(result, loose);
	}
	result.Set(StatsInfo::CAN_HAVE_VALID_VALUES);
	if (!has_nonnull_input) {
		// the result is NULL only if all inputs are NULL
		result.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	}
	return result.ToUnique();
}

template <class LEAST_GREATER_OP>
void LeastGreatestResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	LogicalType child_type = ExpressionBinder::GetExpressionReturnType(input.GetArgument(0));
	for (idx_t i = 1; i < input.GetArgumentCount(); i++) {
		auto arg_type = ExpressionBinder::GetExpressionReturnType(input.GetArgument(i));
		if (!LogicalType::TryGetMaxLogicalType(context, child_type, arg_type, child_type,
		                                       input.GetDecimalWidthLimit())) {
			throw BinderException(input.GetArgument(i).GetQueryLocation(),
			                      "Cannot combine types of %s and %s - an explicit cast is required",
			                      child_type.ToString(), arg_type.ToString());
		}
	}
	switch (child_type.id()) {
	case LogicalTypeId::UNKNOWN:
		throw ParameterNotResolvedException();
	case LogicalTypeId::INTEGER_LITERAL:
		child_type = IntegerLiteral::GetType(child_type);
		break;
	case LogicalTypeId::STRING_LITERAL:
		child_type = LogicalType::VARCHAR;
		break;
	default:
		break;
	}
	using OP = typename LEAST_GREATER_OP::OP;
	switch (child_type.InternalType()) {
#if !DUCKDB_SMALLER_BINARY(least_greatest_types)
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		bound_function.SetFunctionCallback(LeastGreatestFunction<int8_t, OP>);
		break;
	case PhysicalType::INT16:
		bound_function.SetFunctionCallback(LeastGreatestFunction<int16_t, OP>);
		break;
	case PhysicalType::INT32:
		bound_function.SetFunctionCallback(LeastGreatestFunction<int32_t, OP>);
		break;
	case PhysicalType::INT64:
		bound_function.SetFunctionCallback(LeastGreatestFunction<int64_t, OP>);
		break;
	case PhysicalType::INT128:
		bound_function.SetFunctionCallback(LeastGreatestFunction<hugeint_t, OP>);
		break;
	case PhysicalType::UINT8:
		bound_function.SetFunctionCallback(LeastGreatestFunction<uint8_t, OP>);
		break;
	case PhysicalType::UINT16:
		bound_function.SetFunctionCallback(LeastGreatestFunction<uint16_t, OP>);
		break;
	case PhysicalType::UINT32:
		bound_function.SetFunctionCallback(LeastGreatestFunction<uint32_t, OP>);
		break;
	case PhysicalType::UINT64:
		bound_function.SetFunctionCallback(LeastGreatestFunction<uint64_t, OP>);
		break;
	case PhysicalType::UINT128:
		bound_function.SetFunctionCallback(LeastGreatestFunction<uhugeint_t, OP>);
		break;
	case PhysicalType::FLOAT:
		bound_function.SetFunctionCallback(LeastGreatestFunction<float, OP>);
		break;
	case PhysicalType::DOUBLE:
		bound_function.SetFunctionCallback(LeastGreatestFunction<double, OP>);
		break;
	case PhysicalType::VARCHAR:
		bound_function.SetFunctionCallback(LeastGreatestFunction<string_t, OP, StandardLeastGreatest<true>>);
		break;
#endif
	default:
		// fallback with sort keys
		bound_function.SetFunctionCallback(LeastGreatestFunction<string_t, OP, SortKeyLeastGreatest>);
		bound_function.SetInitStateCallback(LeastGreatestSortKeyInit<LEAST_GREATER_OP>);
		break;
	}
	for (auto &arg : bound_function.GetArguments()) {
		arg = child_type;
	}
	bound_function.SetReturnType(child_type);
}

void CollatedLeastGreatestSerialize(Serializer &serializer, const optional_ptr<const FunctionData> bind_data,
                                    const BoundScalarFunction &) {
	serializer.WriteProperty(100, "key", bind_data->Cast<CollatedLeastGreatestData>().key);
}

//! Compares the arguments by the given key instead of by their values
template <class LEAST_GREATER_OP>
void SetCollatedLeastGreatest(BoundScalarFunction &bound_function, const Expression &key) {
	using OP = typename LEAST_GREATER_OP::OP;
	switch (key.GetReturnType().InternalType()) {
#if !DUCKDB_SMALLER_BINARY(least_greatest_types)
	case PhysicalType::UINT64:
		bound_function.SetFunctionCallback(CollatedLeastGreatestFunction<uint64_t, OP>);
		break;
	case PhysicalType::VARCHAR:
		bound_function.SetFunctionCallback(CollatedLeastGreatestFunction<string_t, OP>);
		break;
#endif
	default:
		bound_function.SetFunctionCallback(CollatedLeastGreatestFunction<string_t, OP, SortKeyLeastGreatest>);
		break;
	}
	bound_function.SetInitStateCallback(CollatedLeastGreatestInit<LEAST_GREATER_OP>);
	// the bounds of the arguments do not follow the order of the keys
	bound_function.SetStatisticsCallback(nullptr);
	// the key is kept, binding again outside of a query would compare the values
	bound_function.SetSerializeCallback(CollatedLeastGreatestSerialize);
}

template <class LEAST_GREATER_OP>
unique_ptr<FunctionData> CollatedLeastGreatestDeserialize(Deserializer &deserializer,
                                                          BoundScalarFunction &bound_function) {
	auto key = deserializer.ReadProperty<unique_ptr<Expression>>(100, "key");
	SetCollatedLeastGreatest<LEAST_GREATER_OP>(bound_function, *key);
	return make_uniq<CollatedLeastGreatestData>(std::move(key));
}

//! A type whose values do not compare as they are stored (INTERVAL, TIMETZ, a collated VARCHAR, or a nested type that
//! holds one) is compared by its collation key, as comparisons and ORDER BY do
template <class LEAST_GREATER_OP>
unique_ptr<FunctionData> LeastGreatestBind(BindScalarFunctionInput &input) {
	// an index key and a function that is bound outside of a query compare the values, as the stored keys were built
	if (input.GetArguments().size() < 2 || !input.HasBinder() || input.GetBinder().IsBindingIndexKey()) {
		return nullptr;
	}
	auto &bound_function = input.GetBoundFunction();
	auto &argument_type = bound_function.GetReturnType();
	unique_ptr<Expression> key = make_uniq<BoundReferenceExpression>(argument_type, idx_t(0));
	if (!ExpressionBinder::PushCollation(input.GetClientContext(), key, argument_type)) {
		return nullptr;
	}
	SetCollatedLeastGreatest<LEAST_GREATER_OP>(bound_function, *key);
	return make_uniq<CollatedLeastGreatestData>(std::move(key));
}

template <class OP>
ScalarFunction GetLeastGreatestFunction() {
	ScalarFunction fun({{"arg1", LogicalType::ANY}}, LogicalType::ANY, nullptr, nullptr,
	                   PropagateLeastGreatestStats<OP>, nullptr, LogicalType::ANY, FunctionStability::CONSISTENT,
	                   FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetResolveTypesCallback(LeastGreatestResolveTypes<OP>);
	fun.SetBindCallback(LeastGreatestBind<OP>);
	// only a function that compares by a key is serialized with its data, and is then not bound again
	fun.SetDeserializeCallback(CollatedLeastGreatestDeserialize<OP>);
	return fun;
}

template <class OP>
ScalarFunctionSet GetLeastGreatestFunctions() {
	ScalarFunctionSet fun_set;
	fun_set.AddFunction(GetLeastGreatestFunction<OP>());
	return fun_set;
}

} // namespace

ScalarFunctionSet LeastFun::GetFunctions() {
	return GetLeastGreatestFunctions<LeastOp>();
}

ScalarFunctionSet GreatestFun::GetFunctions() {
	return GetLeastGreatestFunctions<GreaterOp>();
}

} // namespace duckdb
