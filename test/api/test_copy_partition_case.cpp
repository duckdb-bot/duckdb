#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "utf8proc.hpp"

using namespace duckdb;

namespace {

string Lowercase(const string &name) {
	string result(LowerLength(name.c_str(), name.size()), '\0');
	LowerCase(name.c_str(), name.size(), &result[0]);
	return result;
}

//! Names that are equal in uppercase, such as 'σ', 'ς' and 'Σ', are one name
string Uppercase(const string &name) {
	string result;
	auto data = const_data_ptr_cast(name.c_str());
	auto size = NumericCast<utf8proc_ssize_t>(name.size());
	for (utf8proc_ssize_t position = 0; position < size;) {
		utf8proc_int32_t codepoint;
		position += utf8proc_iterate(data + position, size - position, &codepoint);
		utf8proc_uint8_t encoded[4];
		auto length = utf8proc_encode_char(utf8proc_toupper(codepoint), encoded);
		result.append(char_ptr_cast(encoded), NumericCast<idx_t>(length));
	}
	return result;
}

//! Names that are equal after full case folding, such as 'Straße' and 'STRASSE', are one name
string Fold(const string &name) {
	utf8proc_uint8_t *folded = nullptr;
	auto length = utf8proc_map(const_data_ptr_cast(name.c_str()), NumericCast<utf8proc_ssize_t>(name.size()), &folded,
	                           utf8proc_option_t(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));
	if (length < 0) {
		throw InternalException("Failed to fold \"%s\"", name);
	}
	string result(char_ptr_cast(folded), NumericCast<idx_t>(length));
	free(folded);
	return result;
}

enum class CaseMode { PRESERVE, LOWERCASE, FOLD, NO_IDENTITY, STATS_THROW, NO_UPPERCASE_IDENTITY };

//! A file system under "casetest://" that ignores case, by uppercasing names (and keeping or lowercasing them) or by
//! folding them, or that tells case apart but reports no directory identities. Its operations run one at a time.
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
		return local->ListFiles(Resolve(path), callback, opener);
	}
	optional<FileMetadata> GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) override {
		lock_guard<mutex> guard(lock);
		if (mode == CaseMode::STATS_THROW) {
			throw IOException("Cannot get the stats of \"%s\"", file.path);
		}
		auto metadata = local->GetStatsIfExists(OpenFileInfo(Resolve(file.path)), opener);
		auto uppercase = file.path.find('A') != string::npos;
		if (metadata && (mode == CaseMode::NO_IDENTITY || (mode == CaseMode::NO_UPPERCASE_IDENTITY && uppercase))) {
			metadata->device_id = optional_idx();
			metadata->file_id = optional_idx();
		}
		return metadata;
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
	//! The local path of a name, matched to an existing entry regardless of case if the file system ignores case
	string Resolve(const string &path) {
		auto result = root;
		for (auto &component : StringUtil::Split(path.substr(string(PREFIX).size()), "/")) {
			auto name = mode == CaseMode::LOWERCASE ? Lowercase(component) : component;
			if (mode == CaseMode::PRESERVE || mode == CaseMode::LOWERCASE) {
				local->ListFiles(result, [&](const string &entry, bool) {
					if (Uppercase(entry) == Uppercase(component)) {
						name = entry;
					}
				});
			} else if (mode == CaseMode::FOLD) {
				local->ListFiles(result, [&](const string &entry, bool) {
					if (Fold(entry) == Fold(component)) {
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

void RequireCaseCollision(Connection &con, const string &query) {
	INFO(query);
	auto result = con.Query(query);
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "differ only in case"));
}

} // namespace

TEST_CASE("COPY with PARTITION_BY refuses partitions that a file system ignoring case puts in one directory",
          "[copy][api]") {
	for (auto mode : {CaseMode::PRESERVE, CaseMode::LOWERCASE, CaseMode::FOLD}) {
		INFO("mode " << static_cast<int>(mode));
		auto root = TestCreatePath("copy_partition_case");
		TestDeleteDirectory(root);
		TestCreateDirectory(root);
		DuckDB db(nullptr);
		Connection con(db);
		RegisterCaseTestFileSystem(con, root, mode);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT * FROM (VALUES ('a', 1, 1), ('A', 2, 1), ('b', 3, 2), "
		                          "('B', 4, 3)) v(k, x, j)"));

		// the partitions 'a' and 'A' would write the same file
		duckdb::vector<string> collisions {
		    "COPY t TO 'casetest://out' (FORMAT parquet, PARTITION_BY (k))",
		    "COPY t TO 'casetest://out' (FORMAT csv, PARTITION_BY (k))",
		    "COPY t TO 'casetest://out' (FORMAT parquet, PARTITION_BY (j, k))",
		    // letters beyond ASCII in a PARTITION_PATH
		    "COPY (SELECT * FROM (VALUES ('\xC3\xA4', 1), ('\xC3\x84', 2)) v(k, x)) TO 'casetest://out' (FORMAT "
		    "parquet, PARTITION_BY (k), PARTITION_PATH k)",
		    "COPY (SELECT * FROM (VALUES ('\xCF\x83', 1), ('\xCF\x82', 2)) v(k, x)) TO 'casetest://out' (FORMAT "
		    "parquet, PARTITION_BY (k), PARTITION_PATH k)"};
		auto partition_path_copy = [](const string &values) {
			return "COPY (SELECT * FROM (VALUES " + values +
			       ") v(k, x)) TO 'casetest://out' (FORMAT parquet, PARTITION_BY (k), PARTITION_PATH k)";
		};
		if (mode == CaseMode::FOLD) {
			// equal when folded, but not when uppercased letter by letter
			collisions.push_back(partition_path_copy("('Stra\xC3\x9F"
			                                         "e', 1), ('STRASSE', 2)"));
		} else {
			// equal when uppercased, but not when folded
			collisions.push_back(partition_path_copy("('\xC4\xB1', 1), ('I', 2)"));
			collisions.push_back(partition_path_copy("('\xC4\xB1\xCC\x81', 1), ('I\xCC\x81', 2)"));
		}
		for (auto &query : collisions) {
			RequireCaseCollision(con, query);
			TestDeleteDirectory(root);
			TestCreateDirectory(root);
		}
#ifndef DUCKDB_NO_THREADS
		// partition directories created by several threads
		REQUIRE_NO_FAIL(con.Query("SET threads = 8"));
		RequireCaseCollision(con, "COPY (SELECT CASE WHEN i % 2 = 0 THEN 'a' ELSE 'A' END AS k, i FROM range(200000) "
		                          "t(i)) TO 'casetest://out' (FORMAT parquet, PARTITION_BY (k))");
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
	auto copy =
	    "COPY (SELECT * FROM (VALUES ('a', 1), ('A', 2)) v(k, x)) TO '" + out + "' (FORMAT parquet, PARTITION_BY (k))";
	if (ignores_case) {
		RequireCaseCollision(con, copy);
	} else {
		REQUIRE_NO_FAIL(con.Query(copy));
		auto result =
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

TEST_CASE("COPY with PARTITION_BY writes partitions that differ only in case where directories have no identity",
          "[copy][api]") {
	auto root = TestCreatePath("copy_partition_case_no_identity");
	TestDeleteDirectory(root);
	TestCreateDirectory(root);
	TestCreateDirectory(root + "/out");
	TestCreateDirectory(root + "/out/k=a");
	TestCreateDirectory(root + "/out/k=A");
	auto fs = FileSystem::CreateLocal();
	if (!fs->DirectoryExists(root + "/out/K=A")) {
		for (auto mode : {CaseMode::NO_IDENTITY, CaseMode::STATS_THROW, CaseMode::NO_UPPERCASE_IDENTITY}) {
			INFO("mode " << static_cast<int>(mode));
			DuckDB db(nullptr);
			Connection con(db);
			RegisterCaseTestFileSystem(con, root, mode);
			REQUIRE_NO_FAIL(con.Query("COPY (SELECT * FROM (VALUES ('a', 1), ('A', 2)) v(k, x)) TO 'casetest://out' "
			                          "(FORMAT parquet, PARTITION_BY (k), OVERWRITE_OR_IGNORE)"));
			auto result = con.Query("SELECT count(*) FROM read_parquet('" + root + "/out/**/*.parquet')");
			REQUIRE(CHECK_COLUMN(result, 0, {2}));
		}
	}
	TestDeleteDirectory(root);
}
