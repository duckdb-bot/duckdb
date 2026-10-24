#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/file_system.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace duckdb;

// ATTACH OR REPLACE of the attached path reads a file that was replaced on disk (duckdb/duckdb#20748)

#if defined(__linux__) || defined(__APPLE__)
namespace {

//! Whether another process can lock the file for writing, which the read lock of an attached database prevents
bool OtherProcessCanLockForWriting(const string &path) {
	auto pid = fork();
	if (pid == 0) {
		int fd = open(path.c_str(), O_RDWR);
		struct flock lock {};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		_exit(fd >= 0 && fcntl(fd, F_SETLK, &lock) == 0 ? 0 : 1);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int64_t DatabaseOid(Connection &con) {
	auto result = con.Query("SELECT database_oid FROM duckdb_databases() WHERE database_name = 'r'");
	REQUIRE_NO_FAIL(*result);
	return result->Collection().GetValue(0, 0).GetValue<int64_t>();
}

} // namespace

TEST_CASE("Test ATTACH OR REPLACE of a database file that was replaced", "[api]") {
	auto path = TestCreatePath("or_replace_replaced_file.db");
	auto new_path = TestCreatePath("or_replace_replaced_file_new.db");
	DeleteDatabase(path);
	DeleteDatabase(new_path);
	DuckDB db(nullptr);
	Connection con(db);
	auto &fs = FileSystem::GetFileSystem(*con.context);

	REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS w"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE w.t AS SELECT 1 AS i"));
	REQUIRE_NO_FAIL(con.Query("DETACH w"));
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS r (READ_ONLY)"));
	auto result = con.Query("SELECT i FROM r.t");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	auto oid = DatabaseOid(con);

	// the same file keeps its attachment, and with it the lock that keeps writers out
	REQUIRE_NO_FAIL(con.Query("ATTACH OR REPLACE '" + path + "' AS r (READ_ONLY)"));
	REQUIRE(DatabaseOid(con) == oid);
	REQUIRE(!OtherProcessCanLockForWriting(path));

	// a writer replaces the file with a new version
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + new_path + "' AS w"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE w.t AS SELECT 2 AS i, 'new' AS s"));
	REQUIRE_NO_FAIL(con.Query("DETACH w"));
	fs.MoveFile(new_path, path);

	REQUIRE_NO_FAIL(con.Query("ATTACH OR REPLACE '" + path + "' AS r (READ_ONLY)"));
	REQUIRE(DatabaseOid(con) != oid);
	result = con.Query("SELECT i, s FROM r.t");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	REQUIRE(CHECK_COLUMN(result, 1, {"new"}));
	REQUIRE(!OtherProcessCanLockForWriting(path));
	REQUIRE_NO_FAIL(con.Query("DETACH r"));
	REQUIRE(OtherProcessCanLockForWriting(path));
}
#endif
