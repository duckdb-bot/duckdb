#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/mutex.hpp"

using namespace duckdb;

namespace {

enum class CaseMode { PRESERVE, LOWERCASE, SENSITIVE };

//! A file system under "casetest://" that ignores case, either keeping the case a name was created in, as APFS and
//! NTFS do, or storing every name in lowercase, or that tells case apart but cannot list directories. Its operations
//! run one at a time, so that resolving a name and creating it cannot interleave with another one
class CaseTestFileSystem : public FileSystem {
public:
	CaseTestFileSystem(string root_p, CaseMode mode_p)
	    : root(std::move(root_p)), mode(mode_p), local(FileSystem::CreateLocal()) {
	}

	string GetName() const override {
		return "CaseTestFileSystem";
	}
	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, PREFIX);
	}
	bool IsManuallySet() override {
		return true;
	}
	string PathSeparator(const string &) override {
		return "/";
	}
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override {
		lock_guard<mutex> guard(lock);
		return local->OpenFile(Resolve(path), flags, opener);
	}
	bool FileExists(const string &path, optional_ptr<FileOpener> opener) override {
		lock_guard<mutex> guard(lock);
		return local->FileExists(Resolve(path), opener);
	}
	bool IsPipe(const string &, optional_ptr<FileOpener>) override {
		return false;
	}
	bool DirectoryExists(const string &path, optional_ptr<FileOpener> opener) override {
		lock_guard<mutex> guard(lock);
		return local->DirectoryExists(Resolve(path), opener);
	}
	bool CreateDirectoryExtended(const string &path, const CreateDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener) override {
		lock_guard<mutex> guard(lock);
		return local->CreateDirectoryExtended(Resolve(path), options, opener);
	}
	bool RemoveDirectoryExtended(const string &path, const RemoveDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener) override {
		lock_guard<mutex> guard(lock);
		return local->RemoveDirectoryExtended(Resolve(path), options, opener);
	}
	bool ListFiles(const string &path, const std::function<void(const string &, bool)> &callback,
	               FileOpener *opener) override {
		lock_guard<mutex> guard(lock);
		if (mode == CaseMode::SENSITIVE) {
			return false;
		}
		return local->ListFiles(Resolve(path), callback, opener);
	}
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) override {
		lock_guard<mutex> guard(lock);
		local->MoveFile(Resolve(source), Resolve(target), opener);
	}
	void RemoveFile(const string &path, optional_ptr<FileOpener> opener) override {
		lock_guard<mutex> guard(lock);
		local->RemoveFile(Resolve(path), opener);
	}
	bool TryRemoveFile(const string &path, optional_ptr<FileOpener> opener) override {
		lock_guard<mutex> guard(lock);
		return local->TryRemoveFile(Resolve(path), opener);
	}

	static constexpr const char *PREFIX = "casetest://";

private:
	//! The local path of a name, matched to an existing entry regardless of case
	string Resolve(const string &path) {
		auto result = root;
		for (auto &component : StringUtil::Split(path.substr(string(PREFIX).size()), "/")) {
			auto name = mode == CaseMode::LOWERCASE ? StringUtil::Lower(component) : component;
			if (mode != CaseMode::SENSITIVE) {
				local->ListFiles(result, [&](const string &entry, bool) {
					if (StringUtil::CIEquals(entry, component)) {
						name = entry;
					}
				});
			}
			result = local->JoinPath(result, name);
		}
		return result;
	}

private:
	mutex lock;
	string root;
	CaseMode mode;
	unique_ptr<FileSystem> local;
};

void RegisterCaseTestFileSystem(Connection &con, const string &root, CaseMode mode) {
	FileSystem::GetFileSystem(*con.context).RegisterSubSystem(make_uniq<CaseTestFileSystem>(root, mode));
}

} // namespace

TEST_CASE("COPY with PARTITION_BY refuses partitions that a file system ignoring case puts in one directory",
          "[copy][api]") {
	for (auto mode : {CaseMode::PRESERVE, CaseMode::LOWERCASE}) {
		INFO("lowercase " << (mode == CaseMode::LOWERCASE));
		auto root = TestCreatePath("copy_partition_case");
		TestDeleteDirectory(root);
		TestCreateDirectory(root);
		DuckDB db(nullptr);
		Connection con(db);
		RegisterCaseTestFileSystem(con, root, mode);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT * FROM (VALUES ('a', 1, 1), ('A', 2, 1), ('b', 3, 2), "
		                          "('B', 4, 3)) v(k, x, j)"));

		// the partitions 'a' and 'A' would write the same file
		for (auto &options : {"FORMAT parquet, PARTITION_BY (k)", "FORMAT csv, PARTITION_BY (k)",
		                      "FORMAT parquet, PARTITION_BY (j, k)"}) {
			INFO(options);
			auto result = con.Query(string("COPY t TO 'casetest://out' (") + options + ")");
			REQUIRE(result->HasError());
			REQUIRE(StringUtil::Contains(result->GetError(), "differ only in case"));
			TestDeleteDirectory(root);
			TestCreateDirectory(root);
		}

#ifndef DUCKDB_NO_THREADS
		// partitions that threads create at the same time
		REQUIRE_NO_FAIL(con.Query("SET threads = 8"));
		auto parallel_result =
		    con.Query("COPY (SELECT CASE WHEN i % 2 = 0 THEN 'a' ELSE 'A' END AS k, i FROM range(200000) t(i)) "
		              "TO 'casetest://out' (FORMAT parquet, PARTITION_BY (k))");
		REQUIRE(parallel_result->HasError());
		REQUIRE(StringUtil::Contains(parallel_result->GetError(), "differ only in case"));
		TestDeleteDirectory(root);
		TestCreateDirectory(root);
#endif

		// 'b' and 'B' under different parents, and partitions that differ in more than case
		duckdb::vector<std::pair<string, int64_t>> copies {
		    {"COPY (FROM t WHERE x > 2) TO 'casetest://out' (FORMAT parquet, PARTITION_BY (j, k))", 7},
		    {"COPY (FROM t WHERE x IN (1, 3)) TO 'casetest://out' (FORMAT parquet, PARTITION_BY (k))", 4}};
		for (auto &copy : copies) {
			INFO(copy.first);
			REQUIRE_NO_FAIL(con.Query(copy.first));
			auto result = con.Query("SELECT count(*), sum(x) FROM read_parquet('" + root + "/out/**/*.parquet')");
			REQUIRE(CHECK_COLUMN(result, 0, {2}));
			REQUIRE(CHECK_COLUMN(result, 1, {copy.second}));
			TestDeleteDirectory(root);
			TestCreateDirectory(root);
		}
		TestDeleteDirectory(root);
	}
}

TEST_CASE("COPY with PARTITION_BY on the local file system writes partitions that differ only in case unless it "
          "ignores case",
          "[copy][api]") {
	auto root = TestCreatePath("copy_partition_case_local");
	TestDeleteDirectory(root);
	TestCreateDirectory(root);
	auto fs = FileSystem::CreateLocal();
	fs->CreateDirectory(fs->JoinPath(root, "probe"));
	bool ignores_case = fs->DirectoryExists(fs->JoinPath(root, "PROBE"));

	DuckDB db(nullptr);
	Connection con(db);
	auto out = fs->JoinPath(root, "out");
	auto result = con.Query("COPY (SELECT * FROM (VALUES ('a', 1), ('A', 2)) v(k, x)) TO '" + out +
	                        "' (FORMAT parquet, PARTITION_BY (k))");
	if (ignores_case) {
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "differ only in case"));
	} else {
		REQUIRE_NO_FAIL(*result);
		result =
		    con.Query("SELECT k, x FROM read_parquet('" + out + "/**/*.parquet', hive_partitioning = true) ORDER BY x");
		REQUIRE(CHECK_COLUMN(result, 0, {"a", "A"}));
		REQUIRE(CHECK_COLUMN(result, 1, {1, 2}));
		// both directories exist already
		REQUIRE_NO_FAIL(con.Query("COPY (SELECT * FROM (VALUES ('a', 3), ('A', 4)) v(k, x)) TO '" + out +
		                          "' (FORMAT parquet, PARTITION_BY (k), OVERWRITE_OR_IGNORE)"));
		result =
		    con.Query("SELECT k, x FROM read_parquet('" + out + "/**/*.parquet', hive_partitioning = true) ORDER BY x");
		REQUIRE(CHECK_COLUMN(result, 0, {"a", "A"}));
		REQUIRE(CHECK_COLUMN(result, 1, {3, 4}));
	}
	TestDeleteDirectory(root);
}

TEST_CASE("COPY with PARTITION_BY writes partitions that differ only in case where directories cannot be listed",
          "[copy][api]") {
	auto root = TestCreatePath("copy_partition_case_unlistable");
	TestDeleteDirectory(root);
	TestCreateDirectory(root);
	TestCreateDirectory(root + "/out");
	TestCreateDirectory(root + "/out/k=a");
	TestCreateDirectory(root + "/out/k=A");
	auto fs = FileSystem::CreateLocal();
	if (!fs->DirectoryExists(root + "/out/K=A")) {
		DuckDB db(nullptr);
		Connection con(db);
		RegisterCaseTestFileSystem(con, root, CaseMode::SENSITIVE);
		REQUIRE_NO_FAIL(con.Query("COPY (SELECT * FROM (VALUES ('a', 1), ('A', 2)) v(k, x)) TO 'casetest://out' "
		                          "(FORMAT parquet, PARTITION_BY (k), OVERWRITE_OR_IGNORE)"));
		auto result = con.Query("SELECT count(*) FROM read_parquet('" + root + "/out/**/*.parquet')");
		REQUIRE(CHECK_COLUMN(result, 0, {2}));
	}
	TestDeleteDirectory(root);
}
