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
		DuckDB db(nullptr);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
		REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET \"a%2Fb\" (TYPE http)"));
		REQUIRE(SecretFiles(secret_dir) == vector<string> {"a%252Fb.duckdb_secret"});
	}
	// a file written before names were escaped is named after the secret itself
	local.MoveFile(local.JoinPath(secret_dir, "a%252Fb.duckdb_secret"),
	               local.JoinPath(secret_dir, "a%2Fb.duckdb_secret"));

	DuckDB db(nullptr);
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
	REQUIRE_NO_FAIL(con.Query("DROP PERSISTENT SECRET IF EXISTS \"a/b\""));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"a%2Fb.duckdb_secret"});
	result = con.Query("SELECT name FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"a%2Fb"}));
}

TEST_CASE("A file that cannot be restored on rollback does not stop the other files from being restored",
          "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_obstructed");
	LocalFileSystem local;
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	for (auto name : {"s1", "s2", "s3"}) {
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("CREATE PERSISTENT SECRET %s (TYPE http)", name)));
	}

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	for (auto name : {"s1", "s2", "s3"}) {
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("DROP PERSISTENT SECRET %s", name)));
	}
	REQUIRE(SecretFiles(secret_dir).empty());
	// a directory where the file of s2 has to be rewritten
	auto obstruction = local.JoinPath(secret_dir, "s2.duckdb_secret");
	local.CreateDirectory(obstruction);

	// the rollback restores the catalog and the files of s1 and s3, and reports the file of s2
	auto result = con.Query("ROLLBACK");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "s2.duckdb_secret"));
	result = con.Query("SELECT list_sort(list(name)) FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::LIST({Value("s1"), Value("s2"), Value("s3")})}));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret", "s2.duckdb_secret", "s3.duckdb_secret"});
	REQUIRE(local.DirectoryExists(obstruction));

	// once the directory is gone the secret can be written again
	local.RemoveDirectory(obstruction);
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET s2 (TYPE http)"));
	REQUIRE(local.FileExists(obstruction));
}
