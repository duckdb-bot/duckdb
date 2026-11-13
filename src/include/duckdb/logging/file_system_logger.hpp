//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/logging/file_system_logger.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

#define DUCKDB_LOG_FILE_SYSTEM_BYTES(HANDLE, OP, BYTES, POS)                                                           \
	{                                                                                                                  \
		if (HANDLE.logger) {                                                                                           \
			DUCKDB_LOG(HANDLE.logger, FileSystemLogType, HANDLE, OP, BYTES, POS)                                       \
		}                                                                                                              \
	}
#define DUCKDB_LOG_FILE_SYSTEM(HANDLE, OP)                                                                             \
	{                                                                                                                  \
		if (HANDLE.logger) {                                                                                           \
			DUCKDB_LOG(HANDLE.logger, FileSystemLogType, HANDLE, OP)                                                   \
		}                                                                                                              \
	}

// Macros for logging to file handles
#define DUCKDB_LOG_FILE_SYSTEM_READ(HANDLE, BYTES, POS)  DUCKDB_LOG_FILE_SYSTEM_BYTES(HANDLE, "READ", BYTES, POS);
#define DUCKDB_LOG_FILE_SYSTEM_WRITE(HANDLE, BYTES, POS) DUCKDB_LOG_FILE_SYSTEM_BYTES(HANDLE, "WRITE", BYTES, POS);
#define DUCKDB_LOG_FILE_SYSTEM_OPEN(HANDLE)              DUCKDB_LOG_FILE_SYSTEM(HANDLE, "OPEN");
// a handle is also closed by its destructor, which cannot throw, and building the log message allocates, which fails
// when the heap is exhausted, so a close is logged only as far as memory allows
#define DUCKDB_LOG_FILE_SYSTEM_CLOSE(HANDLE)                                                                           \
	{                                                                                                                  \
		try {                                                                                                          \
			DUCKDB_LOG_FILE_SYSTEM(HANDLE, "CLOSE");                                                                   \
		} catch (...) {                                                                                                \
		}                                                                                                              \
	}

} // namespace duckdb
