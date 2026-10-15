#include "catch.hpp"
#include "test_helpers.hpp"

#if defined(__linux__)
#include <dirent.h>
#include <sys/resource.h>
#endif

using namespace duckdb;

#if defined(__linux__)
static idx_t CountOpenFileDescriptors() {
	idx_t count = 0;
	auto dir = opendir("/proc/self/fd");
	if (!dir) {
		return 0;
	}
	while (readdir(dir)) {
		count++;
	}
	closedir(dir);
	return count;
}
#endif

TEST_CASE("Read-ahead keeps the files it opened ahead of a scan within its window", "[api][.]") {
#if defined(__linux__)
	DuckDB db(nullptr);
	Connection con(db);
	auto dir = TestCreatePath("read_ahead_open_files");
	REQUIRE_NO_FAIL(con.Query("SET partitioned_write_max_open_files = 100"));
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT range AS i, range AS p FROM range(3000)) TO '" + dir +
	                          "' (FORMAT parquet, PARTITION_BY p)"));
	REQUIRE_NO_FAIL(con.Query("SET threads = 8"));
	REQUIRE_NO_FAIL(con.Query("SET async_threads = 32"));

	// leave room for the read-ahead window of 64 files, each with a few handles, but not for every file
	struct rlimit original;
	REQUIRE(getrlimit(RLIMIT_NOFILE, &original) == 0);
	struct rlimit limited = original;
	limited.rlim_cur = MinValue<rlim_t>(original.rlim_cur, CountOpenFileDescriptors() + 240);
	REQUIRE(setrlimit(RLIMIT_NOFILE, &limited) == 0);
	auto result = con.Query("SELECT count(*) FROM read_parquet('" + dir + "/*/*.parquet')");
	REQUIRE(setrlimit(RLIMIT_NOFILE, &original) == 0);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {3000}));
#endif
}
