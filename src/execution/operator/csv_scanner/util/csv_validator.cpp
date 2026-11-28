#include "duckdb/execution/operator/csv_scanner/csv_validator.hpp"
#include "duckdb/common/helper.hpp"
#include <sstream>

namespace duckdb {

void ThreadLines::Insert(idx_t thread_idx, ValidatorLine line_info) {
	D_ASSERT(thread_lines.find(thread_idx) == thread_lines.end());
	thread_lines.insert({thread_idx, line_info});
}

string ThreadLines::Print() const {
	string result;
	for (auto &line : thread_lines) {
		result += "{start_pos: " + std::to_string(line.second.start_pos) +
		          ", end_pos: " + std::to_string(line.second.end_pos) + "}";
	}
	return result;
}

[[noreturn]] static void ThrowFullReadNotSupported() {
	std::ostringstream error;
	error << "The Parallel CSV Reader currently does not support a full read on this file." << '\n';
	error << "To correctly parse this file, please run with the single threaded error (i.e., parallel = "
	         "false)"
	      << '\n';
	throw NotImplementedException(error.str());
}

void ThreadLines::Verify() const {
	bool initialized = false;
	idx_t last_end_pos = 0;
	for (auto &line_info : thread_lines) {
		if (!initialized) {
			// First run, we just set the initialized to true
			initialized = true;
		} else {
			if (line_info.second.start_pos == line_info.second.end_pos) {
				last_end_pos = line_info.second.end_pos;
				continue;
			}
			if (last_end_pos + error_margin < line_info.second.start_pos ||
			    line_info.second.start_pos + error_margin < last_end_pos) {
				ThrowFullReadNotSupported();
			}
		}
		last_end_pos = line_info.second.end_pos;
	}
}

void ThreadLines::VerifyReturnedRows() const {
	idx_t last_row_end_pos = 0;
	for (auto &line_info : thread_lines) {
		auto &line = line_info.second;
		if (!line.first_row_pos.IsValid()) {
			continue;
		}
		if (line.first_row_pos.GetIndex() + error_margin < last_row_end_pos) {
			// the same bytes were returned in the rows of two scanners
			ThrowFullReadNotSupported();
		}
		last_row_end_pos = MaxValue(last_row_end_pos, line.last_row_end_pos);
	}
}

void CSVValidator::Insert(idx_t thread_idx, ValidatorLine line_info) {
	thread_lines.Insert(thread_idx, line_info);
}

void CSVValidator::Verify() const {
	thread_lines.Verify();
}

void CSVValidator::VerifyReturnedRows() const {
	thread_lines.VerifyReturnedRows();
}

string CSVValidator::Print() const {
	return thread_lines.Print();
}

} // namespace duckdb
