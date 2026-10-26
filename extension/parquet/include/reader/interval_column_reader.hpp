//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/interval_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_reader.hpp"
#include "duckdb/common/likely.hpp"
#include "reader/templated_column_reader.hpp"
#include "parquet_reader.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Interval Column Reader
//===--------------------------------------------------------------------===//
struct IntervalValueConversion {
	static constexpr const idx_t PARQUET_INTERVAL_SIZE = 12;

	static interval_t ReadParquetInterval(const_data_ptr_t input) {
		interval_t result;
		result.months = Load<int32_t>(input);
		result.micros = int64_t(Load<uint32_t>(input + sizeof(uint32_t) * 2)) * Interval::MICROS_PER_MSEC;
		const auto days = Load<uint32_t>(input + sizeof(uint32_t));
		if (DUCKDB_UNLIKELY(days > UnsafeNumericCast<uint32_t>(NumericLimits<int32_t>::Maximum()))) {
			return MoveDaysPastMaximum(result, days);
		}
		result.days = UnsafeNumericCast<int32_t>(days);
		return result;
	}

	//! The days field is unsigned, and the writer moves the whole days of the microseconds into it when the
	//! milliseconds do not fit, so the days past the largest days of an interval go back into the microseconds
	static interval_t MoveDaysPastMaximum(interval_t result, uint32_t days);

	template <bool CHECKED>
	static interval_t PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			plain_data.available(PARQUET_INTERVAL_SIZE);
		}
		auto res = ReadParquetInterval(const_data_ptr_cast(plain_data.ptr));
		plain_data.unsafe_inc(PARQUET_INTERVAL_SIZE);
		return res;
	}

	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			plain_data.inc(PARQUET_INTERVAL_SIZE);
		} else {
			plain_data.unsafe_inc(PARQUET_INTERVAL_SIZE);
		}
	}

	static bool PlainAvailable(const ByteBuffer &plain_data, const idx_t count) {
		return plain_data.check_available(count * PARQUET_INTERVAL_SIZE);
	}

	static idx_t PlainConstantSize() {
		return 0;
	}
};

class IntervalColumnReader : public TemplatedColumnReader<interval_t, IntervalValueConversion> {
public:
	IntervalColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
	    : TemplatedColumnReader<interval_t, IntervalValueConversion>(reader, schema) {
	}
};

} // namespace duckdb
