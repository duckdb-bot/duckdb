#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "test_helpers.hpp"

#include <algorithm>

using namespace duckdb;

namespace {

vector<string> SecretFiles(const string &secret_dir) {
	auto &local = TestLocalFileSystem();
	vector<string> result;
	if (!local.DirectoryExists(secret_dir)) {
		return result;
	}
	local.ListFiles(secret_dir, [&](const string &name, bool is_dir) { result.push_back(name); });
	std::sort(result.begin(), result.end());
	return result;
}

} // namespace

TEST_CASE("A rolled back statement does not remove the legacy file of another secret", "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_legacy");
	auto &local = TestLocalFileSystem();
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
	auto &local = TestLocalFileSystem();
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
	auto &local = TestLocalFileSystem();
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

TEST_CASE("A failed secret write that changes no file leaves nothing for rollback to repair", "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_failed_write");
	auto &local = TestLocalFileSystem();
	{
		auto config = GetTestConfig();
		DuckDB db(nullptr, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
		REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET \"a%2Fb\" (TYPE http, SCOPE 'http://old')"));
	}
	// the file of the secret has the name used before secret names were escaped
	auto legacy_file = local.JoinPath(secret_dir, "a%2Fb.duckdb_secret");
	local.MoveFile(local.JoinPath(secret_dir, "a%252Fb.duckdb_secret"), legacy_file);
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	auto result = con.Query("SELECT scope[1] FROM duckdb_secrets() WHERE name = 'a%2Fb'");
	REQUIRE(CHECK_COLUMN(result, 0, {"http://old"}));

	// a directory where the file of a new secret goes, so the write fails after nothing was changed
	auto obstruction = local.JoinPath(secret_dir, "s1.duckdb_secret");
	local.CreateDirectory(obstruction);
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	result = con.Query("CREATE PERSISTENT SECRET s1 (TYPE http)");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "s1.duckdb_secret"));
	// the rollback has nothing to repair, and leaves the directory where the file never arrived
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(local.DirectoryExists(obstruction));
	result = con.Query("SELECT count(*) FROM duckdb_secrets() WHERE name = 's1'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));

	// a replacement is renamed over the secret's own file, where a directory is now in its way
	local.RemoveFile(legacy_file);
	local.CreateDirectory(legacy_file);
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	result = con.Query("CREATE OR REPLACE PERSISTENT SECRET \"a%2Fb\" (TYPE http, SCOPE 'http://new')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "a%2Fb.duckdb_secret"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(local.DirectoryExists(legacy_file));
	result = con.Query("SELECT scope[1] FROM duckdb_secrets() WHERE name = 'a%2Fb'");
	REQUIRE(CHECK_COLUMN(result, 0, {"http://old"}));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"a%2Fb.duckdb_secret", "s1.duckdb_secret"});

	// once the directories are gone both secrets are written, the replaced one to its own file
	local.RemoveDirectory(obstruction);
	local.RemoveDirectory(legacy_file);
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE http)"));
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET \"a%2Fb\" (TYPE http, SCOPE 'http://new')"));
	REQUIRE(local.FileExists(legacy_file));
	REQUIRE(local.FileExists(obstruction));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"a%2Fb.duckdb_secret", "s1.duckdb_secret"});
	result = con.Query("SELECT scope[1] FROM duckdb_secrets() WHERE name = 'a%2Fb'");
	REQUIRE(CHECK_COLUMN(result, 0, {"http://new"}));
}

TEST_CASE("A failed secret removal that changes no file leaves nothing for rollback to repair", "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_failed_removal");
	auto &local = TestLocalFileSystem();
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE http)"));

	// a directory with content takes the place of the secret's file, so the removal fails after nothing was changed
	auto secret_file = local.JoinPath(secret_dir, "s1.duckdb_secret");
	auto content = local.JoinPath(secret_file, "content");
	local.RemoveFile(secret_file);
	local.CreateDirectory(secret_file);
	local.OpenFile(content, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	auto result = con.Query("DROP PERSISTENT SECRET s1");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Failed to remove secret file"));
	// the rollback has nothing to repair, and leaves the directory that was not removed
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(local.FileExists(content));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	result = con.Query("SELECT count(*) FROM duckdb_secrets() WHERE name = 's1'");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	// once the directory is gone the secret is written to its file again, and a drop removes that file
	local.RemoveDirectory(secret_file);
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE http)"));
	REQUIRE(local.FileExists(secret_file));
	REQUIRE_NO_FAIL(con.Query("DROP PERSISTENT SECRET s1"));
	REQUIRE(SecretFiles(secret_dir).empty());
}

TEST_CASE("A failed drop keeps the legacy file of the secret for the next drop", "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_legacy_drop");
	auto &local = TestLocalFileSystem();
	{
		auto config = GetTestConfig();
		DuckDB db(nullptr, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
		REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET \"a%2Fb\" (TYPE http)"));
	}
	// the file of the secret has the name used before secret names were escaped
	auto legacy_file = local.JoinPath(secret_dir, "a%2Fb.duckdb_secret");
	local.MoveFile(local.JoinPath(secret_dir, "a%252Fb.duckdb_secret"), legacy_file);
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	auto result = con.Query("SELECT name FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"a%2Fb"}));

	// the file is kept elsewhere while a directory with content is in its place, so the removal fails
	auto kept_file = TestCreatePath("persistent_secret_rollback_legacy_drop_kept");
	local.MoveFile(legacy_file, kept_file);
	local.CreateDirectory(legacy_file);
	local.OpenFile(local.JoinPath(legacy_file, "content"),
	               FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	result = con.Query("DROP PERSISTENT SECRET \"a%2Fb\"");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Failed to remove secret file"));
	// the failed drop writes no file under the escaped name
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"a%2Fb.duckdb_secret"});
	result = con.Query("SELECT name FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"a%2Fb"}));

	// with the file back in its place, the next drop removes it and not the file the name encodes to
	local.RemoveDirectory(legacy_file);
	local.MoveFile(kept_file, legacy_file);
	REQUIRE_NO_FAIL(con.Query("DROP PERSISTENT SECRET \"a%2Fb\""));
	REQUIRE(SecretFiles(secret_dir).empty());
	result = con.Query("SELECT count(*) FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}

TEST_CASE("A rollback does not write back the file of a secret that another instance dropped", "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_dropped_elsewhere");
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE http)"));
	{
		// another instance on the directory drops the secret, which removes its file
		auto other_config = GetTestConfig();
		DuckDB other_db(nullptr, other_config.get());
		Connection other_con(other_db);
		REQUIRE_NO_FAIL(other_con.Query("SET secret_directory = '" + secret_dir + "'"));
		REQUIRE_NO_FAIL(other_con.Query("DROP PERSISTENT SECRET s1"));
	}
	REQUIRE(SecretFiles(secret_dir).empty());

	// the drop finds no file to remove, so whatever it reports there is no file for the rollback to restore
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	con.Query("DROP PERSISTENT SECRET s1");
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(SecretFiles(secret_dir).empty());
	auto result = con.Query("SELECT count(*) FROM duckdb_secrets() WHERE name = 's1'");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	// neither does a drop outside a transaction leave a file
	con.Query("DROP PERSISTENT SECRET s1");
	REQUIRE(SecretFiles(secret_dir).empty());

	// a new instance does not read the secret that was dropped
	auto fresh_config = GetTestConfig();
	DuckDB fresh_db(nullptr, fresh_config.get());
	Connection fresh_con(fresh_db);
	REQUIRE_NO_FAIL(fresh_con.Query("SET secret_directory = '" + secret_dir + "'"));
	result = fresh_con.Query("SELECT count(*) FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
}

TEST_CASE("Rolling back a secret replacement restores the old secret in the same legacy file", "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_legacy_replacement");
	auto &local = TestLocalFileSystem();
	{
		auto config = GetTestConfig();
		DuckDB db(nullptr, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
		REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET \"a%2Fb\" (TYPE http, SCOPE 'http://old')"));
	}
	// the file of the secret has the name used before secret names were escaped
	local.MoveFile(local.JoinPath(secret_dir, "a%252Fb.duckdb_secret"),
	               local.JoinPath(secret_dir, "a%2Fb.duckdb_secret"));
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));

	// the replacement is renamed over the legacy file, and the rollback writes the old secret back into it
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET \"a%2Fb\" (TYPE http, SCOPE 'http://new')"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"a%2Fb.duckdb_secret"});
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"a%2Fb.duckdb_secret"});
	auto result = con.Query("SELECT scope[1] FROM duckdb_secrets() WHERE name = 'a%2Fb'");
	REQUIRE(CHECK_COLUMN(result, 0, {"http://old"}));

	// a new instance reads the old secret from the legacy file
	auto fresh_config = GetTestConfig();
	DuckDB fresh_db(nullptr, fresh_config.get());
	Connection fresh_con(fresh_db);
	REQUIRE_NO_FAIL(fresh_con.Query("SET secret_directory = '" + secret_dir + "'"));
	result = fresh_con.Query("SELECT scope[1] FROM duckdb_secrets() WHERE name = 'a%2Fb'");
	REQUIRE(CHECK_COLUMN(result, 0, {"http://old"}));
}

TEST_CASE("A rollback does not read the other files of the secret directory", "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_unrelated_file");
	auto &local = TestLocalFileSystem();
	{
		auto config = GetTestConfig();
		DuckDB db(nullptr, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
		REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE http)"));
	}
	// an empty file that cannot be read as a secret
	local.OpenFile(local.JoinPath(secret_dir, "unrelated.duckdb_secret"),
	               FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);

	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s2 (TYPE http)"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret", "unrelated.duckdb_secret"});
}

TEST_CASE("A rollback restores the files of secrets changed by a connection that inherited the transaction",
          "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_inherited");
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE http, SCOPE 'http://old')"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s2 (TYPE http)"));

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	{
		// the connection is closed before the transaction it inherited ends
		auto inheriting = Connection::CreateInTransaction(*con.context);
		REQUIRE_NO_FAIL(inheriting.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE http, SCOPE 'http://new')"));
		REQUIRE_NO_FAIL(inheriting.Query("DROP PERSISTENT SECRET s2"));
		REQUIRE_NO_FAIL(inheriting.Query("CREATE PERSISTENT SECRET s3 (TYPE http)"));
	}
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret", "s3.duckdb_secret"});
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret", "s2.duckdb_secret"});

	// a new instance reads the secrets as they were before the transaction
	auto fresh_config = GetTestConfig();
	DuckDB fresh_db(nullptr, fresh_config.get());
	Connection fresh_con(fresh_db);
	REQUIRE_NO_FAIL(fresh_con.Query("SET secret_directory = '" + secret_dir + "'"));
	auto result = fresh_con.Query("SELECT list_sort(list(name)) FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::LIST({Value("s1"), Value("s2")})}));
	result = fresh_con.Query("SELECT scope[1] FROM duckdb_secrets() WHERE name = 's1'");
	REQUIRE(CHECK_COLUMN(result, 0, {"http://old"}));
}

TEST_CASE("A rollback restores the files of secrets when the connection that inherited the transaction leaves it last",
          "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_rollback_inherited_last");
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE http, SCOPE 'http://old')"));

	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s2 (TYPE http)"));
	auto inheriting = Connection::CreateInTransaction(*con.context);
	REQUIRE_NO_FAIL(inheriting.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE http, SCOPE 'http://new')"));
	// the changes are undone when the last connection leaves the transaction, and the files follow then
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret", "s2.duckdb_secret"});
	inheriting = Connection(db);
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});

	// a new instance reads the secret that was not replaced
	auto fresh_config = GetTestConfig();
	DuckDB fresh_db(nullptr, fresh_config.get());
	Connection fresh_con(fresh_db);
	REQUIRE_NO_FAIL(fresh_con.Query("SET secret_directory = '" + secret_dir + "'"));
	auto result = fresh_con.Query("SELECT name, scope[1] FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"s1"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"http://old"}));
}
