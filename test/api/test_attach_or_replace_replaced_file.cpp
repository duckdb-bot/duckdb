#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <condition_variable>
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>
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

void CreateTable(Connection &con, const string &path, const string &select) {
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS w"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE w.t AS " + select));
	REQUIRE_NO_FAIL(con.Query("DETACH w"));
}

//! Holds an open of the database path until a second one arrives or a second passes, so that two connections that
//! found the file replaced at the same time both open it
class GateFileSystem : public LocalFileSystem {
public:
	explicit GateFileSystem(string path_p) : path(std::move(path_p)) {
	}

	bool CanHandleFile(const string &fpath) override {
		return StringUtil::StartsWith(fpath, path);
	}
	unique_ptr<FileHandle> OpenFile(const string &fpath, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override {
		if (gate && fpath == path) {
			std::unique_lock<std::mutex> guard(lock);
			arrivals++;
			arrived.notify_all();
			arrived.wait_for(guard, std::chrono::seconds(1), [&]() { return arrivals >= 2; });
		}
		return LocalFileSystem::OpenFile(fpath, flags, opener);
	}

	string path;
	std::atomic<bool> gate {false};
	std::mutex lock;
	std::condition_variable arrived;
	idx_t arrivals = 0;
};

//! A local file system that reads metadata by opening the file, as the FileSystem default does
class OpeningStatsFileSystem : public LocalFileSystem {
public:
	explicit OpeningStatsFileSystem(string path_p) : path(std::move(path_p)) {
	}

	bool CanHandleFile(const string &fpath) override {
		return StringUtil::StartsWith(fpath, path);
	}
	optional<FileMetadata> GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) override {
		return FileSystem::GetStatsIfExists(file, opener);
	}

	string path;
};

//! Forwards to a local file system, but reads metadata by opening the file as the FileSystem default does
class ForwardingFileSystem : public FileSystem {
public:
	explicit ForwardingFileSystem(string path_p) : path(std::move(path_p)) {
	}

	string GetName() const override {
		return "ForwardingFileSystem";
	}
	bool CanHandleFile(const string &fpath) override {
		return StringUtil::StartsWith(fpath, path);
	}
	unique_ptr<FileHandle> OpenFile(const string &fpath, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override {
		opens++;
		return local.OpenFile(fpath, flags, opener);
	}
	FileMetadata Stats(FileHandle &handle) override {
		return handle.Stats();
	}
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override {
		return local.FileExists(filename, opener);
	}
	string CanonicalizePath(const string &fpath, optional_ptr<FileOpener> opener = nullptr) override {
		return local.CanonicalizePath(fpath, opener);
	}
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return true;
	}

	string path;
	LocalFileSystem local;
	std::atomic<idx_t> opens {0};
};

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

	// without an access mode the new version is attached for reading as well
	oid = DatabaseOid(con);
	CreateTable(con, new_path, "SELECT 3 AS i");
	fs.MoveFile(new_path, path);
	REQUIRE_NO_FAIL(con.Query("ATTACH OR REPLACE '" + path + "' AS r"));
	REQUIRE(DatabaseOid(con) != oid);
	result = con.Query("SELECT i FROM r.t");
	REQUIRE(CHECK_COLUMN(result, 0, {3}));
	REQUIRE_FAIL(con.Query("CREATE TABLE r.u (i INTEGER)"));
	REQUIRE(!OtherProcessCanLockForWriting(path));
	REQUIRE_NO_FAIL(con.Query("DETACH r"));
	REQUIRE(OtherProcessCanLockForWriting(path));
}

TEST_CASE("Test ATTACH OR REPLACE of a replaced database file from two connections at once", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection other(db);
	auto &fs = FileSystem::GetFileSystem(*con.context);
	auto path = fs.CanonicalizePath(TestCreatePath("or_replace_two_connections.db"));
	auto new_path = TestCreatePath("or_replace_two_connections_new.db");
	DeleteDatabase(path);
	DeleteDatabase(new_path);
	auto gate_fs = make_uniq<GateFileSystem>(path);
	auto &gate = *gate_fs;
	fs.RegisterSubSystem(std::move(gate_fs));

	CreateTable(con, path, "SELECT 1 AS i");
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS r (READ_ONLY)"));
	CreateTable(con, new_path, "SELECT 2 AS i");
	fs.MoveFile(new_path, path);

	// each connection that opened the new file would close it again, releasing the lock of the other
	gate.gate = true;
	unique_ptr<QueryResult> results[2];
	std::thread first([&]() { results[0] = con.Query("ATTACH OR REPLACE '" + path + "' AS r (READ_ONLY)"); });
	std::thread second([&]() { results[1] = other.Query("ATTACH OR REPLACE '" + path + "' AS r (READ_ONLY)"); });
	first.join();
	second.join();
	REQUIRE_NO_FAIL(*results[0]);
	REQUIRE_NO_FAIL(*results[1]);
	REQUIRE(gate.arrivals > 0);
	auto result = con.Query("SELECT i FROM r.t");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	REQUIRE(!OtherProcessCanLockForWriting(path));
	REQUIRE_NO_FAIL(con.Query("DETACH r"));
	REQUIRE(OtherProcessCanLockForWriting(path));
}

TEST_CASE("Test ATTACH OR REPLACE of a database file of a file system that opens files to read metadata", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &fs = FileSystem::GetFileSystem(*con.context);
	auto path = fs.CanonicalizePath(TestCreatePath("or_replace_forwarded.db"));
	auto new_path = TestCreatePath("or_replace_forwarded_new.db");
	DeleteDatabase(path);
	DeleteDatabase(new_path);
	CreateTable(con, path, "SELECT 1 AS i");
	auto forwarding_fs = make_uniq<ForwardingFileSystem>(path);
	auto &forwarding = *forwarding_fs;
	fs.RegisterSubSystem(std::move(forwarding_fs));
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS r (READ_ONLY)"));
	REQUIRE(forwarding.opens > 0);
	auto oid = DatabaseOid(con);

	// the file is checked without opening it, as closing it would release the lock of the attached database
	REQUIRE_NO_FAIL(con.Query("ATTACH OR REPLACE '" + path + "' AS r (READ_ONLY)"));
	REQUIRE(DatabaseOid(con) == oid);
	REQUIRE(!OtherProcessCanLockForWriting(path));

	CreateTable(con, new_path, "SELECT 2 AS i");
	fs.MoveFile(new_path, path);
	REQUIRE_NO_FAIL(con.Query("ATTACH OR REPLACE '" + path + "' AS r (READ_ONLY)"));
	REQUIRE(DatabaseOid(con) != oid);
	auto result = con.Query("SELECT i FROM r.t");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	REQUIRE(!OtherProcessCanLockForWriting(path));
	REQUIRE_NO_FAIL(con.Query("DETACH r"));
	REQUIRE(OtherProcessCanLockForWriting(path));
}

TEST_CASE("Test ATTACH OR REPLACE of a database file of a local file system that opens files to read metadata",
          "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &fs = FileSystem::GetFileSystem(*con.context);
	auto path = fs.CanonicalizePath(TestCreatePath("or_replace_opening_stats.db"));
	auto new_path = TestCreatePath("or_replace_opening_stats_new.db");
	DeleteDatabase(path);
	DeleteDatabase(new_path);
	CreateTable(con, path, "SELECT 1 AS i");
	fs.RegisterSubSystem(make_uniq<OpeningStatsFileSystem>(path));
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + path + "' AS r (READ_ONLY)"));
	auto oid = DatabaseOid(con);

	REQUIRE_NO_FAIL(con.Query("ATTACH OR REPLACE '" + path + "' AS r (READ_ONLY)"));
	REQUIRE(DatabaseOid(con) == oid);
	REQUIRE(!OtherProcessCanLockForWriting(path));

	CreateTable(con, new_path, "SELECT 2 AS i");
	fs.MoveFile(new_path, path);
	REQUIRE_NO_FAIL(con.Query("ATTACH OR REPLACE '" + path + "' AS r (READ_ONLY)"));
	REQUIRE(DatabaseOid(con) != oid);
	auto result = con.Query("SELECT i FROM r.t");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	REQUIRE(!OtherProcessCanLockForWriting(path));
	REQUIRE_NO_FAIL(con.Query("DETACH r"));
	REQUIRE(OtherProcessCanLockForWriting(path));
}
#endif
