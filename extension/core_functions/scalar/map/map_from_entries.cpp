#include "duckdb/common/vector/map_vector.hpp"
#include "core_functions/scalar/map_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"

namespace duckdb {

static void MapFromEntriesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	MapUtil::ReinterpretMap(result, args.data[0]);
	MapVector::MapConversionVerify(result, count);
}

//! Builds a map from entries, keeping the first entry of every key and dropping the entries that repeat a key
static void MapFromEntriesKeepFirstFunction(DataChunk &args, ExpressionState &, Vector &result) {
	const auto count = args.size();
	auto &entries = args.data[0];
	auto list_entries = entries.Values<list_entry_t>();
	const auto &keys = MapVector::GetKeys(entries);
	const auto &values = MapVector::GetValues(entries);
	const auto key_validity = keys.Validity();

	// rows can share their list entries, so the kept entries are counted over the rows and not over the children
	idx_t entry_count = 0;
	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto list_entry = list_entries[row_idx];
		if (list_entry.IsValid()) {
			entry_count += list_entry.GetValue().length;
		}
	}
	SelectionVector kept(entry_count);
	idx_t kept_count = 0;
	auto result_entries = FlatVector::Writer<list_entry_t>(result, count);
	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto list_entry = list_entries[row_idx];
		if (!list_entry.IsValid()) {
			result_entries.WriteNull();
			continue;
		}
		const auto &list = list_entry.GetValue();
		const auto offset = kept_count;
		value_set_t unique_keys;
		for (idx_t child_idx = 0; child_idx < list.length; child_idx++) {
			const auto entry_idx = list.offset + child_idx;
			if (!key_validity.IsValid(entry_idx)) {
				MapVector::EvalMapInvalidReason(MapInvalidReason::NULL_KEY);
			}
			if (!unique_keys.insert(keys.GetValue(entry_idx)).second) {
				continue;
			}
			kept.set_index(kept_count++, entry_idx);
		}
		result_entries.WriteValue(list_entry_t(offset, kept_count - offset));
	}

	auto &result_keys = MapVector::GetKeys(result);
	auto &result_values = MapVector::GetValues(result);
	ListVector::SetListSize(result, kept_count);
	result_keys.Slice(keys, kept, kept_count);
	result_keys.Flatten();
	result_values.Slice(values, kept, kept_count);
	result_values.Flatten();
	FlatVector::ValidityMutable(ListVector::GetChildMutable(result)).Resize(kept_count);
	result.Verify();
}

ScalarFunction MapFromEntriesFun::GetFunction() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");
	auto map_type = LogicalType::MAP(key_type, val_type);
	auto row_type = LogicalType::TUPLE({key_type, val_type});

	ScalarFunction fun({}, map_type, MapFromEntriesFunction);
	fun.GetSignature().AddParameter("map", LogicalType::LIST(row_type));
	fun.SetNullHandling(FunctionNullHandling::DEFAULT_NULL_HANDLING);

	fun.SetFallible();
	return fun;
}

ScalarFunction InternalMapFromEntriesKeepFirstFun::GetFunction() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");
	auto map_type = LogicalType::MAP(key_type, val_type);
	auto row_type = LogicalType::TUPLE({key_type, val_type});

	ScalarFunction fun({}, map_type, MapFromEntriesKeepFirstFunction);
	fun.GetSignature().AddParameter("entries", LogicalType::LIST(row_type));
	fun.SetNullHandling(FunctionNullHandling::DEFAULT_NULL_HANDLING);

	fun.SetFallible();
	return fun;
}

} // namespace duckdb
