#include "catch.hpp"
#include "duckdb/common/checksum.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

//! Zero the first two equal non-zero 8-byte words of a buffer, counted from its start, optionally of a given value
bool ZeroTwoEqualWords(data_ptr_t data, idx_t size, optional_idx value = optional_idx()) {
	unordered_map<uint64_t, idx_t> first_offset;
	for (idx_t offset = 0; offset + sizeof(uint64_t) <= size; offset += sizeof(uint64_t)) {
		auto word = Load<uint64_t>(data + offset);
		if (word == 0 || (value.IsValid() && word != value.GetIndex())) {
			continue;
		}
		auto entry = first_offset.find(word);
		if (entry == first_offset.end()) {
			first_offset[word] = offset;
			continue;
		}
		Store<uint64_t>(0, data + entry->second);
		Store<uint64_t>(0, data + offset);
		return true;
	}
	return false;
}

struct ChecksumTestFile {
	unique_ptr<FileHandle> handle;
	unsafe_unique_array<data_t> data;
	idx_t size;
};

ChecksumTestFile ReadChecksumTestFile(FileSystem &fs, const string &path) {
	ChecksumTestFile contents;
	contents.handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE);
	contents.size = contents.handle->GetFileSize();
	contents.data = make_unsafe_uniq_array<data_t>(contents.size);
	contents.handle->Read(QueryContext(), contents.data.get(), contents.size, 0);
	return contents;
}

void WriteChecksumTestFile(ChecksumTestFile &contents) {
	contents.handle->Write(QueryContext(), contents.data.get(), contents.size, 0);
	contents.handle->Sync();
	contents.handle.reset();
}

} // namespace

TEST_CASE("Test functioning of checksum", "[storage]") {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	duckdb::unique_ptr<DuckDB> database;
	auto storage_database = TestCreatePath("checksum_test");
	auto config = GetTestConfig();

	// make sure the database does not exist
	DeleteDatabase(storage_database);
	{
		// create a database and insert values
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE test(a INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES (3);"));
	}
	// we can open the database file now
	REQUIRE_NOTHROW(database = make_uniq<DuckDB>(storage_database, config.get()));
	database.reset();

	// now write random values into the file
	auto handle = fs->OpenFile(storage_database, FileFlags::FILE_FLAGS_WRITE);
	int8_t value = 0x22;
	fs->Write(*handle, &value, sizeof(int8_t), 100);
	handle->Sync();
	handle.reset();
	// reloading the database no longer works
	REQUIRE_THROWS(database = make_uniq<DuckDB>(storage_database, config.get()));

	DeleteDatabase(storage_database);
}

TEST_CASE("Test that the checksum detects two zeroed equal words in a block", "[storage]") {
	auto fs = FileSystem::CreateLocal();
	auto storage_database = TestCreatePath("checksum_equal_words_block");
	auto config = GetTestConfig();
	config->options.storage_compatibility = StorageCompatibility::FromString("v2.0.0");

	DeleteDatabase(storage_database);
	idx_t block_alloc_size;
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("PRAGMA force_compression='uncompressed'"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT (42 + range % 2)::BIGINT AS v FROM range(10000)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		auto result = con.Query("SELECT block_size FROM pragma_database_size()");
		REQUIRE_NO_FAIL(*result);
		block_alloc_size = result->Collection().GetValue(0, 0).GetValue<idx_t>();
	}

	// zero two of the stored values, which are equal words of a data block
	auto contents = ReadChecksumTestFile(*fs, storage_database);
	bool found = false;
	for (idx_t block_start = Storage::FILE_HEADER_SIZE * 3; !found && block_start < contents.size;
	     block_start += block_alloc_size) {
		auto data_start = block_start + Storage::DEFAULT_BLOCK_HEADER_SIZE;
		auto data_end = MinValue<idx_t>(block_start + block_alloc_size, contents.size);
		found = ZeroTwoEqualWords(contents.data.get() + data_start, data_end - data_start, 42);
	}
	REQUIRE(found);
	WriteChecksumTestFile(contents);

	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		auto result = con.Query("SELECT sum(v) FROM t");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "Corrupt database file: computed checksum"));
	}
	DeleteDatabase(storage_database);
}

TEST_CASE("Test that the file headers keep the word XOR checksum", "[storage]") {
	auto fs = FileSystem::CreateLocal();
	auto storage_database = TestCreatePath("checksum_file_headers");
	auto config = GetTestConfig();
	config->options.storage_compatibility = StorageCompatibility::FromString("v2.0.0");

	DeleteDatabase(storage_database);
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range AS v FROM range(10000)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}

	// older releases verify the database headers before they read the storage version
	auto contents = ReadChecksumTestFile(*fs, storage_database);
	REQUIRE(contents.size >= Storage::FILE_HEADER_SIZE * 3);
	for (idx_t header_start = 0; header_start < Storage::FILE_HEADER_SIZE * 3;
	     header_start += Storage::FILE_HEADER_SIZE) {
		auto header = contents.data.get() + header_start;
		auto stored_checksum = Load<uint64_t>(header);
		auto data_size = Storage::FILE_HEADER_SIZE - Storage::DEFAULT_BLOCK_HEADER_SIZE;
		REQUIRE(Checksum(header + Storage::DEFAULT_BLOCK_HEADER_SIZE, data_size, ChecksumType::WORD_XOR) ==
		        stored_checksum);
	}
	contents.handle.reset();
	DeleteDatabase(storage_database);
}

TEST_CASE("Test that the checksum detects two zeroed equal words in a WAL entry", "[storage]") {
	auto fs = FileSystem::CreateLocal();
	auto storage_database = TestCreatePath("checksum_equal_words_wal");
	auto config = GetTestConfig();
	config->options.storage_compatibility = StorageCompatibility::FromString("v2.0.0");
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;

	DeleteDatabase(storage_database);
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (v BIGINT)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT 42 + range % 1 FROM range(1000)"));
	}

	// after an unframed header, the WAL entries are stored as [size][checksum][payload]
	auto contents = ReadChecksumTestFile(*fs, storage_database + ".wal");
	idx_t largest_payload = 0;
	idx_t largest_payload_size = 0;
	for (idx_t header_size = 1; header_size < 256 && largest_payload_size == 0; header_size++) {
		idx_t offset = header_size;
		idx_t payload = 0;
		idx_t payload_size = 0;
		while (offset + 2 * sizeof(uint64_t) <= contents.size) {
			auto entry_size = Load<uint64_t>(contents.data.get() + offset);
			if (entry_size == 0 || entry_size > contents.size - offset - 2 * sizeof(uint64_t)) {
				break;
			}
			if (entry_size > payload_size) {
				payload = offset + 2 * sizeof(uint64_t);
				payload_size = entry_size;
			}
			offset += 2 * sizeof(uint64_t) + entry_size;
		}
		if (offset == contents.size) {
			largest_payload = payload;
			largest_payload_size = payload_size;
		}
	}
	REQUIRE(largest_payload_size > 0);
	// the largest entry holds the inserted values
	REQUIRE(ZeroTwoEqualWords(contents.data.get() + largest_payload, largest_payload_size));
	WriteChecksumTestFile(contents);

	bool success = false;
	try {
		DuckDB db(storage_database, config.get());
		success = true;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Type() == ExceptionType::DATA_CORRUPTION);
		REQUIRE(StringUtil::Contains(error.RawMessage(), "Corrupt WAL file"));
	}
	REQUIRE(!success);
	DeleteDatabase(storage_database);
}

TEST_CASE("Test that a database keeps the checksum of the storage version it was created with", "[storage]") {
	auto storage_database = TestCreatePath("checksum_storage_upgrade");
	auto config = GetTestConfig();

	DeleteDatabase(storage_database);
	{
		config->options.storage_compatibility = StorageCompatibility::FromString("v1.5.0");
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range AS v FROM range(100000)"));
	}
	{
		// raising the storage version keeps the checksum of the blocks that were already written
		config->options.storage_compatibility = StorageCompatibility::FromString("v2.0.0");
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT range FROM range(100000, 200000)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		auto result = con.Query("SELECT count(*), sum(v) FROM t");
		REQUIRE(CHECK_COLUMN(*result, 0, {200000}));
		REQUIRE(CHECK_COLUMN(*result, 1, {Value::HUGEINT(19999900000)}));
	}
	DeleteDatabase(storage_database);
}
