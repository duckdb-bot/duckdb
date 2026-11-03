#include "duckdb/common/radix.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/types/variant_comparison.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"

namespace duckdb {

namespace {

//! Two arrays or two objects being compared, with the needle child to match and the haystack child to try next.
//! Children of objects are taken by raw position, skipping the fields missing in the row
struct VariantCompareFrame {
	VariantCompareFrame(const VariantNode &haystack, const VariantNode &needle, VariantComparisonType category) {
		if (category == VariantComparisonType::OBJECT) {
			haystack_entries.emplace(haystack, VariantIterationOrder::INTERNAL);
			needle_entries.emplace(needle, VariantIterationOrder::INTERNAL);
		} else {
			haystack_elements.emplace(haystack.GetArrayChildren());
			needle_elements.emplace(needle.GetArrayChildren());
		}
	}

	idx_t NeedleCount() const {
		return needle_elements ? needle_elements->size() : needle_entries->GetRawCount();
	}

	//! Finds the next pair of children to compare, or returns false when the needle child has no candidate left
	bool NextPair(VariantNode &haystack_child, VariantNode &needle_child) {
		if (needle_elements) {
			if (needle_index == needle_elements->size() || haystack_index == haystack_elements->size()) {
				return false;
			}
			haystack_child = (*haystack_elements)[haystack_index];
			needle_child = (*needle_elements)[needle_index];
			return true;
		}
		VariantObjectEntry needle_entry;
		for (; needle_index < needle_entries->GetRawCount(); needle_index++) {
			needle_entry = needle_entries->RawEntry(needle_index);
			if (!needle_entry.value.IsMissing()) {
				break;
			}
		}
		if (needle_index == needle_entries->GetRawCount()) {
			return false;
		}
		// a needle entry only matches a haystack entry with the same key
		for (; haystack_index < haystack_entries->GetRawCount(); haystack_index++) {
			auto haystack_entry = haystack_entries->RawEntry(haystack_index);
			if (!haystack_entry.value.IsMissing() && haystack_entry.key == needle_entry.key) {
				haystack_child = haystack_entry.value;
				needle_child = needle_entry.value;
				return true;
			}
		}
		return false;
	}

	optional<VariantArrayIterator> haystack_elements;
	optional<VariantArrayIterator> needle_elements;
	optional<VariantObjectIterator> haystack_entries;
	optional<VariantObjectIterator> needle_entries;
	idx_t needle_index = 0;
	idx_t haystack_index = 0;
};

//! An array or object of the haystack being searched, with the child to search next
struct VariantWalkFrame {
	VariantWalkFrame(const VariantNode &container, VariantLogicalType type_id) {
		if (type_id == VariantLogicalType::OBJECT) {
			entries.emplace(container, VariantIterationOrder::INTERNAL);
		} else {
			elements.emplace(container.GetArrayChildren());
		}
	}

	bool Next(VariantNode &child) {
		if (elements) {
			if (next_index == elements->size()) {
				return false;
			}
			child = (*elements)[next_index++];
			return true;
		}
		while (next_index < entries->GetRawCount()) {
			auto entry = entries->RawEntry(next_index++);
			if (!entry.value.IsMissing()) {
				child = entry.value;
				return true;
			}
		}
		return false;
	}

	optional<VariantArrayIterator> elements;
	optional<VariantObjectIterator> entries;
	idx_t next_index = 0;
};

//! The open arrays and objects of a search, kept on explicit stacks so that deeply nested values cannot overflow the
//! call stack
struct VariantContainsStacks {
	vector<VariantCompareFrame> compare;
	vector<VariantWalkFrame> walk;
};

enum class VariantCompareResult : uint8_t { NOT_EQUAL, EQUAL, OPENED };

bool IsPrimitiveEqual(const VariantNode &haystack, VariantLogicalType haystack_type, const VariantNode &needle,
                      VariantLogicalType needle_type, VariantComparisonType category) {
	switch (category) {
	case VariantComparisonType::NULL_VALUE:
		return true;
	case VariantComparisonType::BOOLEAN:
		return haystack_type == needle_type;
	case VariantComparisonType::UUID:
		return haystack.GetData<hugeint_t>() == needle.GetData<hugeint_t>();
	case VariantComparisonType::REAL:
		// we need to encode so NaN == NaN, -0.0 == 0.0, and infinity == infinity
		return Radix::EncodeDouble(VariantGetRealValue(haystack_type, haystack)) ==
		       Radix::EncodeDouble(VariantGetRealValue(needle_type, needle));
	case VariantComparisonType::NUMBER:
		return VariantGetNumberKey(haystack_type, haystack) == VariantGetNumberKey(needle_type, needle);
	case VariantComparisonType::TIME:
		return VariantGetTimeValue(haystack_type, haystack) == VariantGetTimeValue(needle_type, needle);
	case VariantComparisonType::TIME_TZ:
		return haystack.GetData<dtime_tz_t>() == needle.GetData<dtime_tz_t>();
	case VariantComparisonType::TIMESTAMP:
		return VariantGetTimestampValue(haystack_type, haystack) == VariantGetTimestampValue(needle_type, needle);
	case VariantComparisonType::TIMESTAMP_TZ:
		return VariantGetTimestampTZValue(haystack_type, haystack) == VariantGetTimestampTZValue(needle_type, needle);
	case VariantComparisonType::INTERVAL:
		return haystack.GetData<interval_t>() == needle.GetData<interval_t>();
	case VariantComparisonType::BLOB:
	case VariantComparisonType::BITSTRING:
	case VariantComparisonType::GEOMETRY:
	case VariantComparisonType::VARCHAR:
		return haystack.GetString() == needle.GetString();
	default:
		return false;
	}
}

//! Compares two primitive values, or opens the frame that compares the children of two arrays or two objects
VariantCompareResult CompareOrOpen(const VariantNode &haystack, VariantLogicalType haystack_type,
                                   const VariantNode &needle, VariantLogicalType needle_type,
                                   vector<VariantCompareFrame> &frames) {
	const auto category = GetVariantComparisonType(haystack_type);
	if (category != GetVariantComparisonType(needle_type)) {
		return VariantCompareResult::NOT_EQUAL;
	}
	if (category == VariantComparisonType::ARRAY || category == VariantComparisonType::OBJECT) {
		frames.emplace_back(haystack, needle, category);
		return VariantCompareResult::OPENED;
	}
	return IsPrimitiveEqual(haystack, haystack_type, needle, needle_type, category) ? VariantCompareResult::EQUAL
	                                                                                : VariantCompareResult::NOT_EQUAL;
}

//! Whether the haystack value contains the needle value, every needle child of an array or object being matched by a
//! child of the haystack. Haystack children are not consumed, so one can match several needle children
bool IsEqual(const VariantNode &haystack, VariantLogicalType haystack_type, const VariantNode &needle,
             VariantLogicalType needle_type, vector<VariantCompareFrame> &frames) {
	frames.clear();
	auto result = CompareOrOpen(haystack, haystack_type, needle, needle_type, frames);
	while (!frames.empty()) {
		auto &frame = frames.back();
		if (result == VariantCompareResult::EQUAL) {
			// the needle child is matched, the next one is looked for from the first haystack child
			frame.needle_index++;
			frame.haystack_index = 0;
		} else if (result == VariantCompareResult::NOT_EQUAL) {
			frame.haystack_index++;
		}
		VariantNode haystack_child;
		VariantNode needle_child;
		if (!frame.NextPair(haystack_child, needle_child)) {
			auto all_matched = frame.needle_index == frame.NeedleCount();
			frames.pop_back();
			result = all_matched ? VariantCompareResult::EQUAL : VariantCompareResult::NOT_EQUAL;
			continue;
		}
		// the frame is not used after this, as opening another may move it
		result =
		    CompareOrOpen(haystack_child, haystack_child.GetTypeId(), needle_child, needle_child.GetTypeId(), frames);
	}
	return result == VariantCompareResult::EQUAL;
}

bool IsContainer(VariantLogicalType type_id) {
	return type_id == VariantLogicalType::ARRAY || type_id == VariantLogicalType::OBJECT;
}

//! Whether the haystack value or any value nested in it contains the needle
bool HaystackContains(const VariantNode &haystack, const VariantNode &needle, VariantContainsStacks &stacks) {
	const auto haystack_type = haystack.GetTypeId();
	const auto needle_type = needle.GetTypeId();
	const auto needle_category = GetVariantComparisonType(needle_type);
	if (IsEqual(haystack, haystack_type, needle, needle_type, stacks.compare)) {
		return true;
	}
	auto &walk = stacks.walk;
	walk.clear();
	if (IsContainer(haystack_type)) {
		walk.emplace_back(haystack, haystack_type);
	}
	while (!walk.empty()) {
		auto &frame = walk.back();
		auto opened = false;
		VariantNode child;
		while (!opened && frame.Next(child)) {
			// a value of another category cannot contain the needle, which is checked here as most values differ
			const auto child_type = child.GetTypeId();
			if (GetVariantComparisonType(child_type) == needle_category &&
			    IsEqual(child, child_type, needle, needle_type, stacks.compare)) {
				return true;
			}
			if (IsContainer(child_type)) {
				// the frame is not used after this, as opening another may move it
				walk.emplace_back(child, child_type);
				opened = true;
			}
		}
		if (!opened) {
			walk.pop_back();
		}
	}
	return false;
}

} // namespace

static void VariantContainsFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 2);
	(void)state;

	const auto count = input.size();
	const VectorIterator<VectorVariantType> haystacks(input.data[0]);
	const VectorIterator<VectorVariantType> needles(input.data[1]);

	result.Initialize(VectorDataInitialization::UNINITIALIZED, count);
	auto result_writer = FlatVector::Writer<bool>(result, count);

	VariantContainsStacks stacks;
	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		if (!haystacks.RowIsValid(row_idx) || !needles.RowIsValid(row_idx)) {
			result_writer.WriteNull();
			continue;
		}
		result_writer.WriteValue(HaystackContains(haystacks[row_idx], needles[row_idx], stacks));
	}
}

ScalarFunctionSet VariantContainsFun::GetFunctions() {
	ScalarFunction function("variant_contains", {}, LogicalType::BOOLEAN, VariantContainsFunction);
	function.GetSignature()
	    .AddParameter("variant_haystack", LogicalType::VARIANT())
	    .AddParameter("variant_needle", LogicalType::VARIANT());
	return ScalarFunctionSet(function);
}

} // namespace duckdb
