#include "catch.hpp"
#include "test_helpers.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace duckdb;

#ifndef _WIN32
TEST_CASE("Test the row estimate of read_csv without auto-detection for a file that cannot be inspected", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto fs = FileSystem::CreateLocal();
	auto dir = fs->JoinPath(LocalTestDirectoryPath(), "csv_estimate_unreadable");
	if (!fs->DirectoryExists(dir)) {
		fs->CreateDirectory(dir);
	}
	auto denied_dir = fs->JoinPath(dir, "denied");
	if (!fs->DirectoryExists(denied_dir)) {
		fs->CreateDirectory(denied_dir);
	}
	auto readable_file = fs->JoinPath(dir, "readable.csv");
	auto denied_file = fs->JoinPath(denied_dir, "data.csv");
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT 1 AS id) TO '" + readable_file + "' (HEADER false)"));
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT 2 AS id) TO '" + denied_file + "' (HEADER false)"));

	REQUIRE(chmod(denied_dir.c_str(), 0) == 0);
	struct stat status;
	bool can_inspect = stat(denied_file.c_str(), &status) == 0;
	unique_ptr<QueryResult> result;
	if (!can_inspect) {
		// a list of file entries is not expanded and the filename filter prunes the file without access, so only the
		// row estimate inspects it
		result = con.Query("SELECT sum(id) FROM read_csv([{'filename': '" + readable_file + "'}, {'filename': '" +
		                   denied_file +
		                   "'}], auto_detect = false, header = false, columns = {'id': 'BIGINT'}, filename = true) "
		                   "WHERE filename = '" +
		                   readable_file + "'");
	}
	REQUIRE(chmod(denied_dir.c_str(), 0700) == 0);
	if (can_inspect) {
		// the permissions are not enforced for this user
		return;
	}
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
}
#endif
