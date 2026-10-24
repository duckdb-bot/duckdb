#include "catch.hpp"
#include "test_helpers.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#include <time.h>
#endif

#include <chrono>
#include <thread>

using namespace duckdb;

// A client thread whose query has no task left for it waits instead of polling (duckdb/duckdb#22178)

#if (defined(__linux__) || defined(__APPLE__)) && !defined(DUCKDB_NO_THREADS)
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

} // namespace

TEST_CASE("Test that the client thread does not poll while a worker runs the only task", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads = 4"));
	idx_t worker_runs = 0;
	for (idx_t run = 0; run < 10; run++) {
		// range has a single task, which a worker takes while this thread sleeps
		auto result = con.Submit("SELECT sum(hash(i)) FROM range(50000000) t(i)");
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		auto thread_start = ThreadSeconds();
		auto process_start = ProcessSeconds();
		result->Complete();
		REQUIRE_NO_FAIL(*result);
		auto thread = ThreadSeconds() - thread_start;
		auto others = ProcessSeconds() - process_start - thread;
		if (others < thread) {
			// this thread ran the task itself
			continue;
		}
		worker_runs++;
		// polling keeps this thread busy while the worker does the work, also when they share one CPU
		REQUIRE(thread < 0.2 * others);
	}
	REQUIRE(worker_runs > 0);
}
#endif
