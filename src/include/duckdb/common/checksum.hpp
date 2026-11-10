//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/checksum.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//! The checksum of the blocks and WAL entries of a database file
enum class ChecksumType {
	//! XOR of the hashes of the 8-byte words, which does not detect swapped words or zeroed pairs of equal words
	WORD_XOR,
	//! XXH64, which depends on the position of every byte
	XXHASH64
};

//! Compute a checksum over a buffer of size size
uint64_t Checksum(const uint8_t *buffer, size_t size);
//! Compute a checksum of the given type over a buffer of size size
uint64_t Checksum(const uint8_t *buffer, size_t size, ChecksumType type);

} // namespace duckdb
