#include "catch.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "test_helpers.hpp"

#include <algorithm>

using namespace duckdb;

namespace {

vector<string> SecretFiles(const string &secret_dir) {
	LocalFileSystem local;
	vector<string> result;
	if (!local.DirectoryExists(secret_dir)) {
		return result;
	}
	local.ListFiles(secret_dir, [&](const string &name, bool is_dir) {
		// a write that fails leaves its temporary file behind
		if (!StringUtil::Contains(name, ".tmp-")) {
			result.push_back(name);
		}
	});
	std::sort(result.begin(), result.end());
	return result;
}

} // namespace

TEST_CASE("A rolled back statement does not remove the legacy file of another secret", "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_legacy");
	LocalFileSystem local;
	{
		auto config = GetTestConfig();
		DuckDB db(nullptr, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
		REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET \"a%2Fb\" (TYPE http)"));
		REQUIRE(SecretFiles(secret_dir) == vector<string> {"a%252Fb.duckdb_secret"});
	}
	// a file written before names were escaped is named after the secret itself
	local.MoveFile(local.JoinPath(secret_dir, "a%252Fb.duckdb_secret"),
	               local.JoinPath(secret_dir, "a%2Fb.duckdb_secret"));

	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	auto result = con.Query("SELECT name FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"a%2Fb"}));

	// the name "a/b" encodes to the legacy file's name, so the secret cannot be created; its rollback leaves the file
	result = con.Query("CREATE PERSISTENT SECRET \"a/b\" (TYPE http)");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "belongs to the secret"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"a%2Fb.duckdb_secret"});

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	// the storage is named so that the statement reaches it although it has no such secret
	REQUIRE_NO_FAIL(con.Query("DROP PERSISTENT SECRET IF EXISTS \"a/b\" FROM local_file"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"a%2Fb.duckdb_secret"});
	result = con.Query("SELECT name FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"a%2Fb"}));
}

TEST_CASE("A file that cannot be restored on rollback does not stop the other files from being restored",
          "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_obstructed");
	LocalFileSystem local;
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	vector<string> names {"s1", "s2", "s3"};
	for (auto &name : names) {
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("CREATE PERSISTENT SECRET %s (TYPE http)", name)));
	}
	vector<string> files {"s1.duckdb_secret", "s2.duckdb_secret", "s3.duckdb_secret"};

	// the names are restored in no particular order, so each of them is the one that cannot be restored once
	for (auto &obstructed : names) {
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		for (auto &name : names) {
			REQUIRE_NO_FAIL(con.Query(StringUtil::Format("DROP PERSISTENT SECRET %s", name)));
		}
		REQUIRE(SecretFiles(secret_dir).empty());
		// a directory where the file has to be rewritten
		auto obstruction = local.JoinPath(secret_dir, obstructed + ".duckdb_secret");
		local.CreateDirectory(obstruction);

		// the rollback restores the catalog and the other files, and reports the obstructed file
		auto result = con.Query("ROLLBACK");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), obstructed + ".duckdb_secret"));
		result = con.Query("SELECT list_sort(list(name)) FROM duckdb_secrets()");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::LIST({Value("s1"), Value("s2"), Value("s3")})}));
		REQUIRE(SecretFiles(secret_dir) == files);
		REQUIRE(local.DirectoryExists(obstruction));

		// once the directory is gone the secret can be written again
		local.RemoveDirectory(obstruction);
		REQUIRE_NO_FAIL(
		    con.Query(StringUtil::Format("CREATE OR REPLACE PERSISTENT SECRET %s (TYPE http)", obstructed)));
		REQUIRE(local.FileExists(obstruction));
	}
}

TEST_CASE("A failed commit rolls back every connection state even when a secret file cannot be restored",
          "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_failed_commit");
	LocalFileSystem local;
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE http)"));
	REQUIRE_NO_FAIL(con.Query("SET debug_force_commit_failure = true"));

	// the system catalog is changed before the database, so that the database commits first and fails
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("DROP PERSISTENT SECRET s1"));
	REQUIRE_NO_FAIL(con.Query("CREATE SECRET c1 IN connection (TYPE http)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (i INTEGER)"));
	REQUIRE(SecretFiles(secret_dir).empty());
	auto obstruction = local.JoinPath(secret_dir, "s1.duckdb_secret");
	local.CreateDirectory(obstruction);

	// the commit fails, the file of s1 cannot be restored, and the connection secret is rolled back nevertheless
	auto result = con.Query("COMMIT");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Forced commit failure"));
	REQUIRE(StringUtil::Contains(result->GetError(), "s1.duckdb_secret"));
	REQUIRE_NO_FAIL(con.Query("SET debug_force_commit_failure = false"));
	result = con.Query("SELECT list_sort(list(name)) FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::LIST({Value("s1")})}));
	result = con.Query("SELECT count(*) FROM duckdb_tables() WHERE table_name = 't'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));

	local.RemoveDirectory(obstruction);
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE http)"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
}
