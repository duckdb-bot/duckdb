#include "catch.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>

using namespace duckdb;

namespace {

//! Fails the sync of secret files while enabled, the way a file system reports a delayed write failure, and the
//! removal of existing secret files while enabled
class FailingSecretSyncFileSystem : public LocalFileSystem {
public:
	void FileSync(FileHandle &handle) override {
		if (fail_secret_sync && StringUtil::Contains(handle.GetPath(), ".duckdb_secret")) {
			throw IOException("Could not fsync file \"%s\": No space left on device", handle.GetPath());
		}
		LocalFileSystem::FileSync(handle);
	}

	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) override {
		if (fail_secret_remove && StringUtil::Contains(filename, ".duckdb_secret") &&
		    LocalFileSystem::FileExists(filename, opener)) {
			throw IOException("Could not remove file \"%s\": Permission denied", filename);
		}
		return LocalFileSystem::TryRemoveFile(filename, opener);
	}

	std::atomic<bool> fail_secret_sync {false};
	std::atomic<bool> fail_secret_remove {false};
};

//! The tokens are checked, which only a database started with this option shows
unique_ptr<DBConfig> UnredactedConfig() {
	auto config = GetTestConfig();
	config->SetOptionByName("allow_unredacted_secrets", Value::BOOLEAN(true));
	return config;
}

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

//! A fresh database on the directory reads the secret with this token
void CheckFreshDatabaseReadsToken(const string &secret_dir, const string &token) {
	auto config = UnredactedConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	auto result = con.Query("SELECT name, persistent, secret_string LIKE '%bearer_token=" + token +
	                        "%' FROM duckdb_secrets(redact := false)");
	REQUIRE(CHECK_COLUMN(result, 0, {"s1"}));
	REQUIRE(CHECK_COLUMN(result, 1, {true}));
	REQUIRE(CHECK_COLUMN(result, 2, {true}));
}

} // namespace

TEST_CASE("A persistent secret whose file cannot be synced is not created", "[api][secrets]") {
	auto failing_fs = make_uniq<FailingSecretSyncFileSystem>();
	auto &fs = *failing_fs;
	auto config = UnredactedConfig();
	config->file_system = make_uniq<VirtualFileSystem>(std::move(failing_fs));
	DuckDB db(nullptr, config.get());
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

	// a replacement whose write fails keeps the old secret in its file
	fs.fail_secret_sync = true;
	result = con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'xyz')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not fsync"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	result = con.Query("SELECT name, secret_string LIKE '%bearer_token=abc%' FROM duckdb_secrets(redact := false)");
	REQUIRE(CHECK_COLUMN(result, 0, {"s1"}));
	REQUIRE(CHECK_COLUMN(result, 1, {true}));

	// the old secret is the one on disk
	fs.fail_secret_sync = false;
	CheckFreshDatabaseReadsToken(secret_dir, "abc");

	// when the half written file cannot be removed either, the write's error is the one reported
	fs.fail_secret_sync = true;
	fs.fail_secret_remove = true;
	result = con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'xyz')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not fsync"));
	fs.fail_secret_sync = false;
	fs.fail_secret_remove = false;
	auto files = SecretFiles(secret_dir);
	REQUIRE(files.size() == 2);
	REQUIRE(files[0] == "s1.duckdb_secret");
	REQUIRE(StringUtil::StartsWith(files[1], "s1.duckdb_secret.tmp-"));
	CheckFreshDatabaseReadsToken(secret_dir, "abc");
	LocalFileSystem local;
	local.RemoveFile(local.JoinPath(secret_dir, files[1]));

	// a replacement that succeeds swaps the file
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'xyz')"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	result = con.Query("SELECT secret_string LIKE '%bearer_token=xyz%' FROM duckdb_secrets(redact := false)");
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	CheckFreshDatabaseReadsToken(secret_dir, "xyz");
}

TEST_CASE("A replacement that the catalog rejects does not touch the committed secret's file", "[api][secrets]") {
	auto config = UnredactedConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	auto secret_dir = TestCreatePath("persistent_secret_rejected_replacement");
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'first')"));

	// a transaction reads the secret, another connection replaces it, and the transaction's own replacement conflicts
	Connection reader(db);
	REQUIRE_NO_FAIL(reader.Query("BEGIN"));
	auto result = reader.Query("SELECT name FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"s1"}));
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'winner')"));
	result = reader.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'loser')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "conflict"));
	REQUIRE_NO_FAIL(reader.Query("ROLLBACK"));

	// the committed replacement is in the catalog and in the file
	result = con.Query("SELECT secret_string LIKE '%bearer_token=winner%' FROM duckdb_secrets(redact := false)");
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	CheckFreshDatabaseReadsToken(secret_dir, "winner");
}

TEST_CASE("A failed replacement through the system transaction keeps the replaced secret", "[api][secrets]") {
	auto failing_fs = make_uniq<FailingSecretSyncFileSystem>();
	auto &fs = *failing_fs;
	auto config = UnredactedConfig();
	config->file_system = make_uniq<VirtualFileSystem>(std::move(failing_fs));
	DuckDB db(nullptr, config.get());
	Connection con(db);
	auto secret_dir = TestCreatePath("persistent_secret_system_transaction");
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));

	// secrets registered with the system transaction, as an extension does, have no transaction undoing a catalog
	// change
	auto &secret_manager = SecretManager::Get(*db.instance);
	auto transaction = CatalogTransaction::GetSystemTransaction(*db.instance);
	auto register_token = [&](const string &token) {
		auto secret = make_uniq<KeyValueSecret>(vector<string> {"http://"}, "http", "config", "s1");
		secret->secret_map["bearer_token"] = Value(token);
		secret_manager.RegisterSecret(transaction, std::move(secret), OnCreateConflict::REPLACE_ON_CONFLICT,
		                              SecretPersistType::PERSISTENT);
	};
	register_token("abc");
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});

	// the replacement's write fails, so the replaced secret stays in the catalog and in its file
	fs.fail_secret_sync = true;
	string error;
	try {
		register_token("xyz");
	} catch (std::exception &ex) {
		error = ex.what();
	}
	fs.fail_secret_sync = false;
	REQUIRE(StringUtil::Contains(error, "Could not fsync"));
	auto result =
	    con.Query("SELECT name, persistent, secret_string LIKE '%bearer_token=abc%' FROM duckdb_secrets(redact "
	              ":= false)");
	REQUIRE(CHECK_COLUMN(result, 0, {"s1"}));
	REQUIRE(CHECK_COLUMN(result, 1, {true}));
	REQUIRE(CHECK_COLUMN(result, 2, {true}));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	CheckFreshDatabaseReadsToken(secret_dir, "abc");
}
