#include "catch.hpp"
#include "test_helpers.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

using namespace duckdb;

// Pipes give their data once, so the CSV files sniffed to combine the schema must not be opened again
// (duckdb/duckdb#19493)

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE("Test read_csv of several pipes", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	vector<string> paths;
	vector<int> read_ends;
	for (auto content : {"c1\n1\n", "c1\n2\n"}) {
		int fds[2];
		REQUIRE(pipe(fds) == 0);
		REQUIRE(write(fds[1], content, strlen(content)) == ssize_t(strlen(content)));
		close(fds[1]);
		read_ends.push_back(fds[0]);
		paths.push_back("'/dev/fd/" + to_string(fds[0]) + "'");
	}
	auto result =
	    con.Query("SELECT c1 FROM read_csv([" + StringUtil::Join(paths, ", ") + "], header = true) ORDER BY c1");
	for (auto fd : read_ends) {
		close(fd);
	}
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
	REQUIRE(result->RowCount() == 2);
}
#endif
