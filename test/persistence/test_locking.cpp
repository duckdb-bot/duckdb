#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb.hpp"
#include "test_helpers.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace duckdb;

#define BOOL_COUNT 3

TEST_CASE("Test write lock with multiple processes", "[persistence][.]") {
	uint64_t *count =
	    (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	*count = 0;

	string dbdir = TestCreatePath("writelocktest");
	DeleteDatabase(dbdir);
	// test write lock
	// fork away a child
	pid_t pid = fork();
	if (pid == 0) {
		// child process
		// open db for writing
		DuckDB db(dbdir);
		Connection con(db);
		// opened db for writing
		// insert some values
		(*count)++;
		con.Query("CREATE TABLE a(i INTEGER)");
		con.Query("INSERT INTO a VALUES(42)");
		while (true) {
			con.Query("SELECT * FROM a");
			usleep(100);
		}
	} else if (pid > 0) {
		duckdb::unique_ptr<DuckDB> db;
		// parent process
		// sleep a bit to wait for child process
		while (*count == 0) {
			usleep(100);
		}
		// try to open db for writing, this should fail
		REQUIRE_THROWS(db = make_uniq<DuckDB>(dbdir));
		// kill the child
		if (kill(pid, SIGKILL) != 0) {
			FAIL();
		}
	}
}

TEST_CASE("Test read lock with multiple processes", "[persistence][.]") {
	uint64_t *count =
	    (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	*count = 0;

	string dbdir = TestCreatePath("readlocktest");
	DeleteDatabase(dbdir);

	// create the database
	{
		DuckDB db(dbdir);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (42)"));
	}
	// test read lock
	pid_t pid = fork();
	DBConfig config;
	config.options.access_mode = AccessMode::READ_ONLY;
	if (pid == 0) {
		// child process
		// open db for reading
		DuckDB db(dbdir, &config);
		Connection con(db);

		(*count)++;
		// query some values
		con.Query("SELECT i+2 FROM a");
		while (true) {
			usleep(100);
			con.Query("SELECT * FROM a");
		}
	} else if (pid > 0) {
		duckdb::unique_ptr<DuckDB> db;
		// parent process
		// sleep a bit to wait for child process
		while (*count == 0) {
			usleep(100);
		}
		// try to open db for writing, this should fail
		REQUIRE_THROWS(db = make_uniq<DuckDB>(dbdir));
		// but opening db for reading should work
		REQUIRE_NOTHROW(db = make_uniq<DuckDB>(dbdir, &config));
		// we can query the database
		Connection con(*db);
		REQUIRE_NO_FAIL(con.Query("SELECT * FROM a"));
		// kill the child
		if (kill(pid, SIGKILL) != 0) {
			FAIL();
		}
	}
}

#ifdef F_OFD_SETLK
//! No worker threads, so that a forked child starts from a single thread
static unique_ptr<DBConfig> SingleThreadConfig(bool read_only = false) {
	auto config = make_uniq<DBConfig>();
	config->options.maximum_threads = 1;
	config->options.async_threads = 0;
	if (read_only) {
		config->options.access_mode = AccessMode::READ_ONLY;
	}
	return config;
}

//! Opens the database in a child process, and returns 0 when the lock refuses it, 1 when it opens and 2 for other
//! errors
static int OpenInChildProcess(const string &dbdir, bool read_only = false) {
	pid_t pid = fork();
	if (pid == 0) {
		int result = 2;
		try {
			auto config = SingleThreadConfig(read_only);
			DuckDB child_db(dbdir, config.get());
			result = 1;
		} catch (std::exception &ex) {
			if (StringUtil::Contains(ex.what(), "Could not set lock on file")) {
				result = 0;
			}
		}
		_exit(result);
	}
	REQUIRE(pid > 0);
	int status = 0;
	REQUIRE(waitpid(pid, &status, 0) == pid);
	REQUIRE(WIFEXITED(status));
	return WEXITSTATUS(status);
}

static void RequireLockError(const string &dbdir, DBConfig &config) {
	duckdb::unique_ptr<DuckDB> db;
	REQUIRE_THROWS_WITH(db = make_uniq<DuckDB>(dbdir, &config), Catch::Contains("Could not set lock on file"));
}

//! Keeps a database open, as the main database of an instance or attached with memory-mapped IO
static unique_ptr<DuckDB> OpenDatabase(const string &dbdir, bool read_only, bool mmap) {
	auto config = SingleThreadConfig(read_only && !mmap);
	if (!mmap) {
		return make_uniq<DuckDB>(dbdir, config.get());
	}
	auto db = make_uniq<DuckDB>(nullptr, config.get());
	Connection con(*db);
	// a small reserve, as a writable mapping extends the file to it
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + dbdir + "' AS m (IO_MODE 'MMAP', " +
	                          (read_only ? "READ_ONLY" : "MMAP_RESERVE_SIZE '1MB'") + ")"));
	return db;
}

TEST_CASE("Test that another open of a database in the same process keeps its write lock", "[persistence]") {
	string dbdir = TestCreatePath("samelocktest");
	auto config = SingleThreadConfig();
	auto read_only_config = SingleThreadConfig(true);
	for (auto mmap : {false, true}) {
		DeleteDatabase(dbdir);
		{ DuckDB db(dbdir, config.get()); }
		{
			auto db = OpenDatabase(dbdir, false, mmap);
			REQUIRE(OpenInChildProcess(dbdir) == 0);

			// another instance in this process conflicts with the lock, and reading the file to refuse it keeps the
			// lock
			RequireLockError(dbdir, *config);
			RequireLockError(dbdir, *read_only_config);
			REQUIRE(OpenInChildProcess(dbdir) == 0);
			REQUIRE(OpenInChildProcess(dbdir, true) == 0);

			// a read of the file from this instance
			Connection con(*db);
			REQUIRE_NO_FAIL(con.Query("SELECT octet_length(content) FROM read_blob('" + dbdir + "')"));
			REQUIRE(OpenInChildProcess(dbdir) == 0);
		}
		// closing the database releases the lock
		REQUIRE(OpenInChildProcess(dbdir) == 1);

		// two read-only opens in this process, closed in either order
		for (idx_t first = 0; first < 2; first++) {
			auto a = OpenDatabase(dbdir, true, mmap);
			auto b = OpenDatabase(dbdir, true, mmap);
			RequireLockError(dbdir, *config);
			(first == 0 ? a : b).reset();
			REQUIRE(OpenInChildProcess(dbdir) == 0);
			REQUIRE(OpenInChildProcess(dbdir, true) == 1);
			a.reset();
			b.reset();
			REQUIRE(OpenInChildProcess(dbdir) == 1);
		}
	}
}

//! Starts a process with exec, and returns once the exec succeeded
static pid_t StartSleepingProcess() {
	int exec_pipe[2];
	REQUIRE(pipe(exec_pipe) == 0);
	REQUIRE(fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC) == 0);
	pid_t pid = fork();
	if (pid == 0) {
		close(exec_pipe[0]);
		execlp("sleep", "sleep", "60", static_cast<char *>(nullptr));
		// the exec failed, report it through the pipe that the exec would have closed
		char failed = 1;
		if (write(exec_pipe[1], &failed, 1) != 1) {
			_exit(3);
		}
		_exit(2);
	}
	close(exec_pipe[1]);
	char failed = 0;
	auto read_count = read(exec_pipe[0], &failed, 1);
	close(exec_pipe[0]);
	REQUIRE(pid > 0);
	REQUIRE(read_count == 0);
	return pid;
}

TEST_CASE("Test that a process started from DuckDB does not keep the database locked", "[persistence]") {
	string dbdir = TestCreatePath("execlocktest");
	DeleteDatabase(dbdir);
	auto config = SingleThreadConfig();
	{ DuckDB db(dbdir, config.get()); }
	for (auto mmap : {false, true}) {
		for (auto read_only : {false, true}) {
			auto db = OpenDatabase(dbdir, read_only, mmap);
			auto pid = StartSleepingProcess();
			db.reset();
			// the database is no longer locked while the started process runs
			auto result = OpenInChildProcess(dbdir);
			int status = 0;
			auto still_running = waitpid(pid, &status, WNOHANG) == 0;
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			REQUIRE(still_running);
			REQUIRE(result == 1);
		}
	}
}
#endif
