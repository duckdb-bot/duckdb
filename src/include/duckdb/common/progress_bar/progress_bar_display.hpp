//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/progress_bar/progress_bar_display.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <chrono>

namespace duckdb {

class ProgressBarDisplay {
public:
	ProgressBarDisplay() {
	}
	virtual ~ProgressBarDisplay() {
	}

public:
	virtual void Update(double percentage) = 0;
	virtual void Finish() = 0;
	//! The time the query started, before binding, as the display is only created after it
	virtual void SetQueryStart(std::chrono::steady_clock::time_point query_start) {
	}
};

} // namespace duckdb
