#include "catch.hpp"
#include "test_helpers.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#include <time.h>
#endif

using namespace duckdb;

// A client thread whose query has no task left for it waits instead of polling (duckdb/duckdb#22178)

#if defined(__linux__) || defined(__APPLE__)
namespace {

double ThreadSeconds() {
	timespec now;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
	return double(now.tv_sec) + double(now.tv_nsec) / 1e9;
}

double ProcessSeconds() {
	rusage usage;
	getrusage(RUSAGE_SELF, &usage);
	return double(usage.ru_utime.tv_sec) + double(usage.ru_utime.tv_usec) / 1e6 + double(usage.ru_stime.tv_sec) +
	       double(usage.ru_stime.tv_usec) / 1e6;
}

double WallSeconds() {
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return double(now.tv_sec) + double(now.tv_nsec) / 1e9;
}

} // namespace

TEST_CASE("Test that the client thread does not poll while a worker runs the only task", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads = 4"));
	for (idx_t run = 0; run < 10; run++) {
		auto wall_start = WallSeconds();
		auto thread_start = ThreadSeconds();
		auto process_start = ProcessSeconds();
		// range has a single task, which either this thread or a worker runs
		auto result = con.Query("SELECT sum(hash(i)) FROM range(20000000) t(i)");
		REQUIRE_NO_FAIL(*result);
		auto wall = WallSeconds() - wall_start;
		auto thread = ThreadSeconds() - thread_start;
		auto others = ProcessSeconds() - process_start - thread;
		// polling keeps this thread busy while another one does the work
		REQUIRE(!(thread > 0.5 * wall && others > 0.5 * wall));
	}
}
#endif
