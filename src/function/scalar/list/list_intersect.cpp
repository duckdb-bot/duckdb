#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/scalar/list_functions.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/helper.hpp"

namespace duckdb {

struct ListIntersectBindData : public FunctionData {
	explicit ListIntersectBindData(unique_ptr<Expression> key_p) : key(std::move(key_p)) {
	}

	//! Maps an element to the key that compares it, empty when the elements are compared as they are
	unique_ptr<Expression> key;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ListIntersectBindData>(key ? key->Copy() : nullptr);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ListIntersectBindData>();
		return Expression::Equals(key, other.key);
	}
	static void Serialize(Serializer &serializer, const optional_ptr<const FunctionData> bind_data,
	                      const BoundScalarFunction &) {
		auto &data = bind_data->Cast<ListIntersectBindData>();
		serializer.WritePropertyWithDefault(100, "key", data.key);
	}
	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &function) {
		auto key = deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(100, "key");
		function.SetReturnType(deserializer.Get<const LogicalType &>());
		return make_uniq<ListIntersectBindData>(std::move(key));
	}
};

//! Creates the sort keys of the elements of a list, from their comparison keys when they have one
static void CreateElementSortKeys(ExpressionState &state, const ListIntersectBindData &bind_data, const Vector &child,
                                  idx_t child_count, Vector &sort_keys) {
	const OrderModifiers order_modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
	if (!bind_data.key) {
		CreateSortKeyHelpers::CreateSortKey(child, order_modifiers, sort_keys);
		return;
	}
	ExpressionExecutor executor(state.GetContext(), *bind_data.key);
	DataChunk elements;
	elements.InitializeEmpty({child.GetType()});
	for (idx_t offset = 0; offset < child_count; offset += STANDARD_VECTOR_SIZE) {
		const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, child_count - offset);
		elements.data[0].Slice(child, offset, offset + count);
		elements.CheckCardinality(count);
		Vector keys(bind_data.key->GetReturnType());
		executor.ExecuteExpression(elements, keys);
		Vector batch_sort_keys(LogicalType::BLOB);
		CreateSortKeyHelpers::CreateSortKey(keys, count, order_modifiers, batch_sort_keys);
		VectorOperations::Copy(batch_sort_keys, sort_keys, count, 0, offset);
	}
}

static void ListIntersectFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto row_count = args.size();

	// Handle NULL return type case
	if (result.GetType() == LogicalType::SQLNULL) {
		ConstantVector::SetNull(result, count_t(row_count));
		return;
	}

	auto &l_vec = args.data[0];
	auto &r_vec = args.data[1];

	const auto l_size = ListVector::GetListSize(l_vec);
	const auto r_size = ListVector::GetListSize(r_vec);

	auto &l_child = ListVector::GetChildMutable(l_vec);
	auto &r_child = ListVector::GetChildMutable(r_vec);

	auto l_entries = l_vec.Values<list_entry_t>();
	auto r_entries = r_vec.Values<list_entry_t>();

	Vector l_sortkey_vec(LogicalType::BLOB, l_size);
	Vector r_sortkey_vec(LogicalType::BLOB, r_size);

	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<ListIntersectBindData>();
	CreateElementSortKeys(state, bind_data, l_child, l_size, l_sortkey_vec);
	CreateElementSortKeys(state, bind_data, r_child, r_size, r_sortkey_vec);

	const auto l_sortkey_ptr = l_sortkey_vec.Values<string_t>();
	const auto r_sortkey_ptr = r_sortkey_vec.Values<string_t>();
	auto l_validity = l_child.Validity();
	auto r_validity = r_child.Validity();

	auto result_data = FlatVector::Writer<list_entry_t>(result, row_count);
	for (idx_t i = 0; i < row_count; i++) {
		auto l_entry = l_entries[i];
		auto r_entry = r_entries[i];

		if (!l_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}

		auto list = result_data.WriteDynamicList();
		if (!r_entry.IsValid()) {
			continue;
		}

		const auto &l_list = l_entry.GetValue();
		const auto &r_list = r_entry.GetValue();

		if (l_list.length == 0 || r_list.length == 0) {
			continue;
		}

		string_set_t set;
		string_set_t result_set;
		string_map_t<idx_t> key_to_index_map;

		// Choose which side to hash and which to iterate
		const bool use_l_for_hash = l_list.length <= r_list.length;
		const auto &hash_list = use_l_for_hash ? l_list : r_list;
		const auto &iter_list = use_l_for_hash ? r_list : l_list;
		const auto &hash_fmt = use_l_for_hash ? l_validity : r_validity;
		const auto &iter_fmt = use_l_for_hash ? r_validity : l_validity;
		const auto &hash_keys = use_l_for_hash ? l_sortkey_ptr : r_sortkey_ptr;
		const auto &iter_keys = use_l_for_hash ? r_sortkey_ptr : l_sortkey_ptr;

		for (idx_t j = 0; j < hash_list.length; j++) {
			const idx_t h_idx = hash_list.offset + j;
			if (!hash_fmt.IsValid(h_idx)) {
				// value is NULL - skip
				continue;
			}
			const auto &key = hash_keys.GetValueUnsafe(h_idx);
			set.insert(key);
			if (!use_l_for_hash) {
				continue;
			}
			if (bind_data.key) {
				// elements with equal keys can differ, the first of them is the one to return
				key_to_index_map.emplace(key, h_idx);
			} else {
				// of equal elements such as 0.0 and -0.0 the last one stays, the keys of an index hold it
				key_to_index_map[key] = h_idx;
			}
		}

		// Iterate the chosen side, but ALWAYS emit a LEFT index
		const idx_t row_max_length = MinValue(l_list.length, r_list.length);
		SelectionVector row_sel(row_max_length);
		idx_t row_result_length = 0;
		for (idx_t j = 0; j < iter_list.length; j++) {
			const idx_t it_idx = iter_list.offset + j;
			if (!iter_fmt.IsValid(it_idx)) {
				// value is NULL - skip
				continue;
			}
			const auto &key = iter_keys.GetValueUnsafe(it_idx);
			if (set.find(key) == set.end() || result_set.find(key) != result_set.end()) {
				continue;
			}
			result_set.insert(key);

			const idx_t emit_left_idx = use_l_for_hash ? key_to_index_map[key] : it_idx;

			row_sel.set_index(row_result_length, emit_left_idx);
			row_result_length++;
		}

		list.Append(l_child, row_sel, l_size, 0, row_result_length);
	}
}
//! Whether the elements match by their comparison keys: not in an index key or outside of a query, where the function
//! is bound as it was without them, so that the keys of an index that exists do not change
template <class INPUT>
static bool ComparesByKeys(INPUT &input) {
	return input.HasBinder() && !input.GetBinder().IsBindingIndexKey();
}

static unique_ptr<FunctionData> ListIntersectBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	D_ASSERT(input.GetBoundFunction().GetArguments().size() == 2);
	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));
	arguments[1] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[1]));

	// both arguments have the type with the combined collations, whose elements match by their comparison keys
	auto &list_type = input.GetBoundFunction().GetArguments()[0];
	unique_ptr<Expression> key;
	if (ComparesByKeys(input) && list_type.id() == LogicalTypeId::LIST) {
		auto &element_type = ListType::GetChildType(list_type);
		key = make_uniq<BoundReferenceExpression>(element_type, idx_t(0));
		if (!ExpressionBinder::PushCollation(context, key, element_type, CollationType::COMBINABLE_COLLATIONS)) {
			key.reset();
		}
	}
	return make_uniq<ListIntersectBindData>(std::move(key));
}

static void ListIntersectResolveTypes(ResolveScalarFunctionTypesInput &input) {
	if (!ComparesByKeys(input)) {
		return;
	}
	// the arguments are cast to types that keep the collations of both lists
	FunctionBinder::CombineArgumentCollations(input.GetBoundFunction(), input.GetArguments());
}

ScalarFunction ListIntersectFun::GetFunction() {
	auto fun =
	    ScalarFunction({}, LogicalType::LIST(LogicalType::TEMPLATE("T")), ListIntersectFunction, ListIntersectBind);
	fun.GetSignature()
	    .AddParameter("list1", LogicalType::LIST(LogicalType::TEMPLATE("T")))
	    .AddParameter("list2", LogicalType::LIST(LogicalType::TEMPLATE("T")));
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetResolveTypesCallback(ListIntersectResolveTypes);
	fun.SetSerializeCallback(ListIntersectBindData::Serialize);
	fun.SetDeserializeCallback(ListIntersectBindData::Deserialize);
	return fun;
}

} // namespace duckdb
