#include "catch.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

idx_t WALSize(FileSystem &fs, const string &wal_path) {
	if (!fs.FileExists(wal_path)) {
		return 0;
	}
	return fs.GetFileSize(*fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ));
}

//! Writes three committed transactions to the WAL and returns the WAL size after each of them
duckdb::vector<idx_t> WriteTransactions(const string &database_path, DBConfig &config) {
	LocalFileSystem fs;
	DeleteDatabase(database_path);
	duckdb::vector<idx_t> transaction_ends;
	DuckDB db(database_path, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (k INTEGER, i INTEGER, s VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	transaction_ends.push_back(WALSize(fs, database_path + ".wal"));
	for (idx_t k = 1; k <= 3; k++) {
		REQUIRE_NO_FAIL(
		    con.Query("INSERT INTO t SELECT " + to_string(k) + ", range, 'value ' || range FROM range(1000)"));
		transaction_ends.push_back(WALSize(fs, database_path + ".wal"));
	}
	return transaction_ends;
}

//! Inverts bytes of the WAL, keeping its size
void DamageBytes(const string &wal_path, idx_t offset, idx_t count) {
	LocalFileSystem fs;
	auto handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE);
	duckdb::vector<data_t> bytes(count);
	handle->Read(QueryContext(), bytes.data(), bytes.size(), offset);
	for (auto &byte : bytes) {
		byte = static_cast<data_t>(~byte);
	}
	handle->Write(QueryContext(), bytes.data(), bytes.size(), offset);
	handle->Sync();
}

//! Inverts a byte in the middle of a transaction's entries, inside the payload of its insert
void DamageTransaction(const string &wal_path, idx_t start, idx_t end) {
	DamageBytes(wal_path, start + (end - start) / 2, 1);
}

duckdb::unique_ptr<DBConfig> TornWALConfig(bool abort_on_wal_failure) {
	auto config = GetTestConfig();
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	config->options.abort_on_wal_failure = abort_on_wal_failure;
	return config;
}

} // namespace

TEST_CASE("A WAL entry with a wrong checksum in the last transaction is a torn tail", "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_last");
	auto wal_path = database_path + ".wal";
	auto ends = WriteTransactions(database_path, *config);
	DamageTransaction(wal_path, ends[2], ends[3]);
	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("SELECT k, count(*) FROM t GROUP BY k ORDER BY k");
		REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
		REQUIRE(CHECK_COLUMN(result, 1, {1000, 1000}));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (4, 42, 'after recovery')"));
	}
	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("SELECT k, count(*) FROM t GROUP BY k ORDER BY k");
		REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 4}));
		REQUIRE(CHECK_COLUMN(result, 1, {1000, 1000, 1}));
	}
	DeleteDatabase(database_path);
}

TEST_CASE("A WAL entry with a wrong checksum before a complete transaction is corruption", "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_middle");
	auto wal_path = database_path + ".wal";
	// damage the first and the second of three transactions, a complete transaction follows each of them
	for (idx_t damaged = 1; damaged <= 2; damaged++) {
		auto ends = WriteTransactions(database_path, *config);
		DamageTransaction(wal_path, ends[damaged - 1], ends[damaged]);
		REQUIRE_THROWS_WITH(DuckDB(database_path, config.get()), Catch::Contains("does not match stored checksum"));
	}
	DeleteDatabase(database_path);
}

TEST_CASE("abort_on_wal_failure rejects a torn WAL entry with a wrong checksum", "[storage][wal]") {
	auto config = TornWALConfig(true);
	auto database_path = TestCreatePath("wal_torn_checksum_abort");
	auto wal_path = database_path + ".wal";
	auto ends = WriteTransactions(database_path, *config);
	DamageTransaction(wal_path, ends[2], ends[3]);
	REQUIRE_THROWS_WITH(DuckDB(database_path, config.get()), Catch::Contains("does not match stored checksum"));
	DeleteDatabase(database_path);
}

TEST_CASE("A WAL entry with a wrong checksum is not a torn tail when more damage follows", "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_twice");
	auto wal_path = database_path + ".wal";
	auto ends = WriteTransactions(database_path, *config);
	// damage the insert of the last transaction and the end of its WAL_FLUSH
	DamageTransaction(wal_path, ends[2], ends[3]);
	DamageBytes(wal_path, ends[3] - 1, 1);
	REQUIRE_THROWS_WITH(DuckDB(database_path, config.get()), Catch::Contains("does not match stored checksum"));
	DeleteDatabase(database_path);
}

TEST_CASE("A WAL entry with a wrong checksum is not a torn tail when a checkpoint WAL follows", "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_checkpoint_wal");
	auto wal_path = database_path + ".wal";
	auto ends = WriteTransactions(database_path, *config);
	DamageTransaction(wal_path, ends[2], ends[3]);
	{
		LocalFileSystem fs;
		auto handle =
		    fs.OpenFile(wal_path + ".checkpoint", FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
		handle->Sync();
	}
	REQUIRE_THROWS_WITH(DuckDB(database_path, config.get()), Catch::Contains("does not match stored checksum"));
	LocalFileSystem().TryRemoveFile(wal_path + ".checkpoint");
	DeleteDatabase(database_path);
}
