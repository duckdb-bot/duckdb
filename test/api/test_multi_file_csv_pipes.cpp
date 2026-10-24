#include "catch.hpp"
#include "test_helpers.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

using namespace duckdb;

// Pipes give their data once, so the CSV files sniffed to combine the schema must not be opened again
// (duckdb/duckdb#19493)

#if defined(__linux__) || defined(__APPLE__)
namespace {

//! Runs the query on read_csv of pipes that give the contents, with header = true
unique_ptr<QueryResult> QueryPipes(Connection &con, const vector<string> &contents, const string &select) {
	vector<string> paths;
	vector<int> read_ends;
	for (auto &content : contents) {
		int fds[2];
		REQUIRE(pipe(fds) == 0);
		REQUIRE(write(fds[1], content.c_str(), content.size()) == ssize_t(content.size()));
		close(fds[1]);
		read_ends.push_back(fds[0]);
		paths.push_back("'/dev/fd/" + to_string(fds[0]) + "'");
	}
	auto result = con.Query(
	    StringUtil::Replace(select, "PIPES", "read_csv([" + StringUtil::Join(paths, ", ") + "], header = true)"));
	for (auto fd : read_ends) {
		close(fd);
	}
	return result;
}

} // namespace

TEST_CASE("Test read_csv of several pipes", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = QueryPipes(con, {"c1\n1\n", "c1\n2\n"}, "SELECT c1 FROM PIPES ORDER BY c1");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
	REQUIRE(result->RowCount() == 2);

	// each pipe is sniffed for its own header and dialect, as files are
	result = QueryPipes(con, {"a,b\n1,10\n", "b,a\n20,2\n"}, "SELECT a, b FROM PIPES ORDER BY a");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
	REQUIRE(CHECK_COLUMN(result, 1, {10, 20}));
	REQUIRE(result->RowCount() == 2);

	result = QueryPipes(con, {"a,b\n1,10\n", "a;b\n2;20\n"}, "SELECT a, b FROM PIPES ORDER BY a");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
	REQUIRE(CHECK_COLUMN(result, 1, {10, 20}));
	REQUIRE(result->RowCount() == 2);

	// an empty pipe gives no rows
	result = QueryPipes(con, {"c1\n1\n", ""}, "SELECT c1 FROM PIPES");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(result->RowCount() == 1);

	// a pipe listed twice gives its data once, as the second reader finds it drained
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	REQUIRE(write(fds[1], "c1\n1\n", 5) == 5);
	close(fds[1]);
	auto path = "'/dev/fd/" + to_string(fds[0]) + "'";
	result = con.Query("SELECT count(*), sum(c1) FROM read_csv([" + path + ", " + path + "], header = true)");
	close(fds[0]);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(CHECK_COLUMN(result, 1, {1}));
}
#endif
