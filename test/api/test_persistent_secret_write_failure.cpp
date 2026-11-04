#include "catch.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/main/config.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>

using namespace duckdb;

namespace {

//! Fails the sync of secret files while enabled, the way a file system reports a delayed write failure
class FailingSecretSyncFileSystem : public LocalFileSystem {
public:
	void FileSync(FileHandle &handle) override {
		if (fail_secret_sync && StringUtil::Contains(handle.GetPath(), ".duckdb_secret")) {
			throw IOException("Could not fsync file \"%s\": No space left on device", handle.GetPath());
		}
		LocalFileSystem::FileSync(handle);
	}

	std::atomic<bool> fail_secret_sync {false};
};

vector<string> SecretFiles(const string &secret_dir) {
	LocalFileSystem local;
	vector<string> result;
	if (!local.DirectoryExists(secret_dir)) {
		return result;
	}
	local.ListFiles(secret_dir, [&](const string &name, bool is_dir) { result.push_back(name); });
	std::sort(result.begin(), result.end());
	return result;
}

} // namespace

TEST_CASE("A persistent secret whose file cannot be synced is not created", "[api][secrets]") {
	auto failing_fs = make_uniq<FailingSecretSyncFileSystem>();
	auto &fs = *failing_fs;
	DBConfig config;
	config.file_system = make_uniq<VirtualFileSystem>(std::move(failing_fs));
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto secret_dir = TestCreatePath("persistent_secret_write_failure");
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));

	// the write fails when the file is synced, so the statement fails and nothing stays behind
	fs.fail_secret_sync = true;
	auto result = con.Query("CREATE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'abc')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not fsync"));
	result = con.Query("SELECT count(*) FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE(SecretFiles(secret_dir).empty());

	// the file system recovers
	fs.fail_secret_sync = false;
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'abc')"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});

	// a replacement whose write fails keeps the old secret, in the catalog and in its file
	fs.fail_secret_sync = true;
	result = con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'xyz')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not fsync"));
	result = con.Query("SELECT name FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"s1"}));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});

	// the old secret is the one on disk
	fs.fail_secret_sync = false;
	DuckDB db2(nullptr, &config);
	Connection con2(db2);
	REQUIRE_NO_FAIL(con2.Query("SET secret_directory = '" + secret_dir + "'"));
	result = con2.Query("SELECT name, persistent FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"s1"}));
	REQUIRE(CHECK_COLUMN(result, 1, {true}));

	// a replacement that succeeds swaps the file
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'xyz')"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
}
