#include "catch.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/main/settings.hpp"
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
	auto &fs = TestLocalFileSystem();
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
	auto &fs = TestLocalFileSystem();
	auto handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE);
	duckdb::vector<data_t> bytes(count);
	handle->Read(QueryContext(), bytes.data(), bytes.size(), offset);
	for (auto &byte : bytes) {
		byte = static_cast<data_t>(~byte);
	}
	handle->Write(QueryContext(), bytes.data(), bytes.size(), offset);
	handle->Sync();
}

//! Overwrites bytes of the WAL with zeros up to its end
void ZeroUntilEnd(const string &wal_path, idx_t offset) {
	auto &fs = TestLocalFileSystem();
	auto handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE);
	duckdb::vector<data_t> zeros(handle->GetFileSize() - offset, 0);
	handle->Write(QueryContext(), zeros.data(), zeros.size(), offset);
	handle->Sync();
}

//! Inverts a byte in the middle of a transaction's entries, inside the payload of its insert
void DamageTransaction(const string &wal_path, idx_t start, idx_t end) {
	DamageBytes(wal_path, start + (end - start) / 2, 1);
}

//! Cuts the WAL off at the given size
void CutWAL(const string &wal_path, idx_t size) {
	auto &fs = TestLocalFileSystem();
	auto handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_WRITE);
	fs.Truncate(*handle, NumericCast<int64_t>(size));
	handle->Sync();
}

//! Counts how often the existence of a checkpoint WAL is checked
class CheckpointWALLookupFileSystem : public LocalFileSystem {
public:
	atomic<idx_t> lookup_count {0};

public:
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener) override {
		if (StringUtil::EndsWith(filename, ".wal.checkpoint")) {
			lookup_count++;
		}
		return LocalFileSystem::FileExists(filename, opener);
	}
};

duckdb::unique_ptr<DBConfig> TornWALConfig(bool abort_on_wal_failure) {
	auto config = GetTestConfig();
	config->options.checkpoint_wal_size = idx_t(-1);
	config->SetOption(CheckpointOnShutdownSetting::SettingIndex, Value::BOOLEAN(false));
	config->SetOption(DebugAbortOnWalFailureSetting::SettingIndex, Value::BOOLEAN(abort_on_wal_failure));
	return config;
}

//! Attaches the database with the alias torn while only the given paths may be accessed
duckdb::unique_ptr<QueryResult> AttachWithAllowedPaths(Connection &con, const string &database_path,
                                                       const duckdb::vector<string> &allowed_paths,
                                                       const string &access_mode) {
	auto path_list =
	    StringUtil::Join(allowed_paths, ", ", [](const string &path) { return Value(path).ToSQLString(); });
	REQUIRE_NO_FAIL(con.Query("SET allowed_paths=[" + path_list + "]"));
	REQUIRE_NO_FAIL(con.Query("SET enable_external_access=false"));
	return con.Query("ATTACH " + Value(database_path).ToSQLString() + " AS torn (" + access_mode + ")");
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
		REQUIRE(result->RowCount() == 2);
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (4, 42, 'after recovery')"));
	}
	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("SELECT k, count(*) FROM t GROUP BY k ORDER BY k");
		REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 4}));
		REQUIRE(CHECK_COLUMN(result, 1, {1000, 1000, 1}));
		REQUIRE(result->RowCount() == 3);
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
		auto &fs = TestLocalFileSystem();
		auto handle =
		    fs.OpenFile(wal_path + ".checkpoint", FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
		handle->Sync();
	}
	REQUIRE_THROWS_WITH(DuckDB(database_path, config.get()), Catch::Contains("does not match stored checksum"));
	TestLocalFileSystem().TryRemoveFile(wal_path + ".checkpoint");
	DeleteDatabase(database_path);
}

TEST_CASE("A WAL entry with a wrong checksum and a damaged size is not a torn tail", "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_size");
	auto wal_path = database_path + ".wal";
	auto ends = WriteTransactions(database_path, *config);
	// the size of the entry grows, so the entries after it are read at the wrong offsets
	DamageBytes(wal_path, ends[1], 1);
	REQUIRE_THROWS_WITH(DuckDB(database_path, config.get()), Catch::Contains("does not match stored checksum"));
	DeleteDatabase(database_path);
}

TEST_CASE("A WAL whose last transaction is zeroed up to the end is a torn tail", "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_zeroed");
	auto wal_path = database_path + ".wal";
	auto ends = WriteTransactions(database_path, *config);
	ZeroUntilEnd(wal_path, ends[2] + (ends[3] - ends[2]) / 2);
	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("SELECT k, count(*) FROM t GROUP BY k ORDER BY k");
		REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
		REQUIRE(CHECK_COLUMN(result, 1, {1000, 1000}));
		REQUIRE(result->RowCount() == 2);
	}
	DeleteDatabase(database_path);
}

TEST_CASE("A WAL entry with a wrong checksum before an empty entry is not a torn tail", "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_empty");
	auto wal_path = database_path + ".wal";
	auto ends = WriteTransactions(database_path, *config);
	DamageTransaction(wal_path, ends[2], ends[3]);
	// the size of the last WAL_FLUSH becomes zero, while its checksum and payload stay
	idx_t flush_entry_size = 2 * sizeof(uint64_t) + 5;
	auto &fs = TestLocalFileSystem();
	{
		auto handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE);
		uint64_t zero = 0;
		handle->Write(QueryContext(), &zero, sizeof(zero), ends[3] - flush_entry_size);
		handle->Sync();
	}
	REQUIRE_THROWS_WITH(DuckDB(database_path, config.get()), Catch::Contains("does not match stored checksum"));
	DeleteDatabase(database_path);
}

TEST_CASE("A WAL entry with a wrong checksum before a cut off entry header is a torn tail", "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_header");
	auto wal_path = database_path + ".wal";
	for (idx_t kept_header_bytes : {1, 8, 15}) {
		auto ends = WriteTransactions(database_path, *config);
		DamageTransaction(wal_path, ends[2], ends[3]);
		// keep only the start of the header of the last WAL_FLUSH
		idx_t flush_entry_size = 2 * sizeof(uint64_t) + 5;
		CutWAL(wal_path, ends[3] - flush_entry_size + kept_header_bytes);
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("SELECT k, count(*) FROM t GROUP BY k ORDER BY k");
		REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
		REQUIRE(CHECK_COLUMN(result, 1, {1000, 1000}));
		REQUIRE(result->RowCount() == 2);
	}
	DeleteDatabase(database_path);
}

TEST_CASE("A damaged WAL_FLUSH drops its transaction and the next one", "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_flush");
	auto wal_path = database_path + ".wal";
	auto ends = WriteTransactions(database_path, *config);
	// damage the WAL_FLUSH of the second transaction, the flush of the third one is the only one after it
	DamageBytes(wal_path, ends[2] - 1, 1);
	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("SELECT k, count(*) FROM t GROUP BY k ORDER BY k");
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
		REQUIRE(CHECK_COLUMN(result, 1, {1000}));
		REQUIRE(result->RowCount() == 1);
	}
	DeleteDatabase(database_path);
}

TEST_CASE("A cut off WAL tail is a torn tail when only the database and its WAL may be accessed", "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_allowed_cut");
	auto wal_path = database_path + ".wal";
	for (auto &access_mode : {"READ_ONLY", "READ_WRITE"}) {
		auto ends = WriteTransactions(database_path, *config);
		// the last WAL_FLUSH loses its last byte
		CutWAL(wal_path, ends[3] - 1);
		DuckDB db(nullptr, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(AttachWithAllowedPaths(con, database_path, {database_path, wal_path}, access_mode));
		auto result = con.Query("SELECT k, count(*) FROM torn.t GROUP BY k ORDER BY k");
		REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
		REQUIRE(CHECK_COLUMN(result, 1, {1000, 1000}));
		REQUIRE(result->RowCount() == 2);
	}
	DeleteDatabase(database_path);
}

TEST_CASE("A WAL entry with a wrong checksum is not a torn tail when the checkpoint WAL may not be accessed",
          "[storage][wal]") {
	auto config = TornWALConfig(false);
	auto database_path = TestCreatePath("wal_torn_checksum_allowed");
	auto wal_path = database_path + ".wal";
	auto ends = WriteTransactions(database_path, *config);
	DamageTransaction(wal_path, ends[2], ends[3]);
	{
		// whether a checkpoint WAL continues the log is unknown
		DuckDB db(nullptr, config.get());
		Connection con(db);
		auto result = AttachWithAllowedPaths(con, database_path, {database_path, wal_path}, "READ_ONLY");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "does not match stored checksum"));
	}
	{
		DuckDB db(nullptr, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(AttachWithAllowedPaths(con, database_path, {database_path, wal_path, wal_path + ".checkpoint"},
		                                       "READ_ONLY"));
		auto result = con.Query("SELECT k, count(*) FROM torn.t GROUP BY k ORDER BY k");
		REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
		REQUIRE(CHECK_COLUMN(result, 1, {1000, 1000}));
		REQUIRE(result->RowCount() == 2);
	}
	DeleteDatabase(database_path);
}

TEST_CASE("A checkpoint WAL is only looked for after a WAL entry with a wrong checksum", "[storage][wal]") {
	auto database_path = TestCreatePath("wal_torn_checksum_lookup");
	auto wal_path = database_path + ".wal";
	for (bool wrong_checksum : {false, true}) {
		auto config = TornWALConfig(false);
		auto ends = WriteTransactions(database_path, *config);
		if (wrong_checksum) {
			DamageTransaction(wal_path, ends[2], ends[3]);
		} else {
			// the last WAL_FLUSH loses its last byte
			CutWAL(wal_path, ends[3] - 1);
		}
		auto file_system = make_uniq<CheckpointWALLookupFileSystem>();
		auto &lookup_count = file_system->lookup_count;
		config->file_system = make_uniq<VirtualFileSystem>(std::move(file_system));
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("SELECT k, count(*) FROM t GROUP BY k ORDER BY k");
		REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
		REQUIRE(CHECK_COLUMN(result, 1, {1000, 1000}));
		REQUIRE(result->RowCount() == 2);
		idx_t expected_lookup_count = wrong_checksum ? 1 : 0;
		REQUIRE(lookup_count.load() == expected_lookup_count);
	}
	DeleteDatabase(database_path);
}
