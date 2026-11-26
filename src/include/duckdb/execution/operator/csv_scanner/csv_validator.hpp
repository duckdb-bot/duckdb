//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_validator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/map.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

//! Information used to validate
struct ValidatorLine {
	ValidatorLine(idx_t start_pos_p, idx_t end_pos_p, optional_idx first_row_pos_p, idx_t last_row_end_pos_p)
	    : start_pos(start_pos_p), end_pos(end_pos_p), first_row_pos(first_row_pos_p),
	      last_row_end_pos(last_row_end_pos_p) {
	}
	const idx_t start_pos;
	const idx_t end_pos;
	//! Where the first row that the scanner returned starts, not set when it returned no row
	const optional_idx first_row_pos;
	//! Where the last row that the scanner returned ends
	const idx_t last_row_end_pos;
};

struct ThreadLines {
	ThreadLines() {};
	//! Validate everything is as it should be, returns true if it's all good, false o.w.
	void Verify() const;
	//! Validate that no scanner returned a row that starts inside a row that a scanner before it returned
	void VerifyReturnedRows() const;

	void Insert(idx_t thread, ValidatorLine line_info);

	string Print() const;

private:
	map<idx_t, ValidatorLine> thread_lines;
	//! We allow up to 2 bytes of error margin (basically \r\n)
	static constexpr idx_t error_margin = 2;
};

//! The validator works by double-checking that threads started and ended in the right positions
struct CSVValidator {
	CSVValidator() {
	}
	//! Validate that all lines are good
	void Verify() const;
	//! Validate the rows that were returned, for scans that skip rows with errors
	void VerifyReturnedRows() const;

	//! Inserts line_info to a given thread index
	void Insert(idx_t thread, ValidatorLine line_info);

	string Print() const;

private:
	//! Thread lines for this file
	ThreadLines thread_lines;
};

} // namespace duckdb
