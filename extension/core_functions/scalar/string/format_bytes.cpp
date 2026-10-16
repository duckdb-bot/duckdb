#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uhugeint.hpp"

namespace duckdb {

template <int64_t MULTIPLIER>
static string FormatUnsignedBytes(uhugeint_t bytes) {
	if (bytes <= uhugeint_t(NumericLimits<idx_t>::Maximum())) {
		return StringUtil::BytesToHumanReadableString(bytes.lower, MULTIPLIER);
	}
	// the largest unit is a PiB, as for smaller values
	uhugeint_t multiplier(MULTIPLIER);
	auto tera = bytes / multiplier / multiplier / multiplier / multiplier;
	auto peta = tera / multiplier;
	auto fractional_part = (tera % multiplier) * uhugeint_t(10) / multiplier;
	return peta.ToString() + "." + fractional_part.ToString() + (MULTIPLIER == 1000 ? " PB" : " PiB");
}

template <int64_t MULTIPLIER>
static void FormatBytesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<hugeint_t, string_t>(args.data[0], result, [&](hugeint_t bytes) {
		bool is_negative = bytes < hugeint_t(0);
		uhugeint_t unsigned_bytes;
		if (bytes == NumericLimits<hugeint_t>::Minimum()) {
			unsigned_bytes = uhugeint_t(1) << 127;
		} else {
			unsigned_bytes = uhugeint_t(is_negative ? -bytes : bytes);
		}
		return heap.AddString((is_negative ? "-" : "") + FormatUnsignedBytes<MULTIPLIER>(unsigned_bytes));
	});
}

ScalarFunction FormatBytesFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::VARCHAR, FormatBytesFunction<1024>);
	fun.GetSignature().AddParameter("integer", LogicalType::HUGEINT);
	return fun;
}

ScalarFunction FormatreadabledecimalsizeFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::VARCHAR, FormatBytesFunction<1000>);
	fun.GetSignature().AddParameter("integer", LogicalType::HUGEINT);
	return fun;
}

} // namespace duckdb
