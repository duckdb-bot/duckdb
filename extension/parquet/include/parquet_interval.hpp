//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_interval.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/interval.hpp"
#include "zstd/common/xxhash.hpp"

namespace duckdb {

struct ParquetIntervalUtils {
	static constexpr const idx_t PARQUET_INTERVAL_SIZE = 12;

	static void Encode(const interval_t &interval, data_ptr_t target) {
		const auto millis = interval.micros / Interval::MICROS_PER_MSEC;
		if (millis > NumericLimits<uint32_t>::Maximum()) {
			// the milliseconds do not fit in their 32 bits
			EncodeWithWholeDays(interval, target);
			return;
		}
		Store<uint32_t>(static_cast<uint32_t>(interval.months), target);
		Store<uint32_t>(NumericCast<uint32_t>(interval.days), target + sizeof(uint32_t));
		Store<uint32_t>(NumericCast<uint32_t>(millis), target + sizeof(uint32_t) * 2);
	}

	static uint64_t HashNormalized(const interval_t &interval) {
		data_t bytes[PARQUET_INTERVAL_SIZE];
		// bloom filters of earlier files hash this encoding, so it does not change
		EncodeWithWholeDays(interval.Normalize(), bytes);
		return duckdb_zstd::XXH64(bytes, sizeof(bytes), 0);
	}

private:
	//! Encodes the interval with the whole days of its time part moved to the days field
	static void EncodeWithWholeDays(const interval_t &interval, data_ptr_t target) {
		const auto days = int64_t(interval.days) + interval.micros / Interval::MICROS_PER_DAY;
		const auto micros = interval.micros % Interval::MICROS_PER_DAY;
		Store<uint32_t>(static_cast<uint32_t>(interval.months), target);
		Store<uint32_t>(NumericCast<uint32_t>(days), target + sizeof(uint32_t));
		Store<uint32_t>(NumericCast<uint32_t>(micros / Interval::MICROS_PER_MSEC), target + sizeof(uint32_t) * 2);
	}
};

} // namespace duckdb
