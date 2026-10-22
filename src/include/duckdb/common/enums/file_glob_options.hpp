//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/file_glob_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

#include <functional>

namespace duckdb {

enum class FileGlobOptions : uint8_t { DISALLOW_EMPTY = 0, ALLOW_EMPTY = 1, FALLBACK_GLOB = 2 };

struct FileGlobInput {
	FileGlobInput(FileGlobOptions options) // NOLINT: allow implicit conversion from FileGlobOptions
	    : behavior(options), allow_empty(options == FileGlobOptions::ALLOW_EMPTY) {
	}
	FileGlobInput(FileGlobOptions options, string extension_p)
	    : behavior(options), allow_empty(options == FileGlobOptions::ALLOW_EMPTY), extension(std::move(extension_p)) {
	}

	bool AllowsEmpty() const {
		return allow_empty || behavior == FileGlobOptions::ALLOW_EMPTY;
	}

	FileGlobOptions behavior;
	bool allow_empty;
	string extension;
	//! If set, whether a relative path may be looked up in a location (the path itself, or the path joined with a
	//! file_search_path entry); a location it refuses is not looked at
	std::function<bool(const string &location)> can_access_location;
};

} // namespace duckdb
