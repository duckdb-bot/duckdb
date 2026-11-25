#include "catch.hpp"
#include "test_helpers.hpp"

#ifndef _WIN32
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace duckdb;

TEST_CASE("A CSV read from a pipe returns rows of a full buffer while the writer stays open", "[api][csv]") {
	constexpr idx_t BUFFER_SIZE = 8192;
	const auto path = TestJoinPath(LocalTestDirectoryPath(), "csv_full_buffer.fifo");
	unlink(path.c_str());
	REQUIRE(mkfifo(path.c_str(), 0600) == 0);
	// a writer that also holds the read end can open and fill the pipe before the query starts
	const auto writer = open(path.c_str(), O_RDWR);
	REQUIRE(writer >= 0);
	// complete rows that fill exactly one buffer
	string rows;
	while (rows.size() < BUFFER_SIZE) {
		rows += "1\n";
	}
	const auto written = write(writer, rows.data(), rows.size());

	DuckDB db(nullptr);
	Connection con(db);
	std::atomic<bool> returned {false};
	std::atomic<bool> has_error {true};
	std::atomic<idx_t> row_count {0};
	std::thread reader([&] {
		auto result = con.Query("SELECT * FROM read_csv('" + path +
		                        "', auto_detect=false, header=false, columns={'a': 'INTEGER'}, buffer_size=" +
		                        to_string(BUFFER_SIZE) + ", parallel=false) LIMIT 1");
		has_error = result->HasError();
		row_count = has_error ? 0 : result->RowCount();
		returned = true;
	});
	// the writer neither writes nor closes until the query has returned
	for (idx_t i = 0; i < 300 && !returned; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	const bool returned_while_open = returned;
	close(writer);
	reader.join();
	unlink(path.c_str());

	REQUIRE(written == NumericCast<int64_t>(rows.size()));
	REQUIRE(returned_while_open);
	REQUIRE(!has_error);
	REQUIRE(row_count == 1);
}
#endif
