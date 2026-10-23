#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "test_helpers.hpp"

#include <set>
#include <utility>

using namespace duckdb;

namespace {

//! The blocks of the table data that readers before v1.4.2 enter and whose next pointer leads to a block that is
//! not in use. Those readers resolve the next pointer of a block as soon as they enter it, so such a pointer makes
//! them fail to open the file.
idx_t CountNextPointersToFreeBlocks(Connection &con, idx_t table_count) {
	con.BeginTransaction();
	auto &catalog = Catalog::GetCatalog(*con.context, "");
	auto &metadata_manager = StorageManager::Get(catalog).GetBlockManager().GetMetadataManager();
	std::set<std::pair<block_id_t, idx_t>> in_use;
	for (auto &info : metadata_manager.GetMetadataInfo()) {
		std::set<idx_t> free_blocks(info.free_list.begin(), info.free_list.end());
		for (idx_t i = 0; i < info.total_blocks; i++) {
			if (free_blocks.find(i) == free_blocks.end()) {
				in_use.emplace(info.block_id, i);
			}
		}
	}
	idx_t result = 0;
	for (idx_t t = 0; t < table_count; t++) {
		auto &table = Catalog::GetEntry<TableCatalogEntry>(
		    *con.context, QualifiedName(INVALID_CATALOG, DEFAULT_SCHEMA, Identifier("candles_" + std::to_string(t))));
		auto &pointers = table.GetStorage().GetRowGroupCollection()->GetMetadataPointers();
		REQUIRE(!pointers.empty());
		for (auto &pointer : pointers) {
			auto metadata_pointer = metadata_manager.FromDiskPointer(pointer);
			auto handle = metadata_manager.Pin(metadata_pointer);
			auto next = Load<idx_t>(handle.handle.GetDataMutable() +
			                        metadata_pointer.index * metadata_manager.GetMetadataBlockSize());
			if (next == idx_t(-1)) {
				continue;
			}
			MetaBlockPointer next_pointer(next, 0);
			if (in_use.find(std::make_pair(next_pointer.GetBlockId(), idx_t(next_pointer.GetBlockIndex()))) ==
			    in_use.end()) {
				result++;
			}
		}
	}
	con.Commit();
	return result;
}

} // namespace

TEST_CASE("Metadata of a file for older versions has no next pointers into free blocks", "[storage]") {
	auto path = TestCreatePath("metadata_chain_old_readers.db");
	DeleteDatabase(path);
	auto config = GetTestConfig();
	config->SetOptionByName("storage_compatibility_version", Value("v1.0.0"));
	// only the explicit checkpoints below write the file, automatic ones would rewrite the tables this relies on
	config->options.checkpoint_wal_size = idx_t(1) << 40;
	config->options.checkpoint_on_shutdown = false;
	{
		DuckDB db(path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE wide AS PIVOT (SELECT i, j, i * j AS v FROM range(1000) t(i), "
		                          "range(100) s(j)) ON j USING first(v) ORDER BY i"));
		REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE narrow AS SELECT i, i * 2 AS b, i * 3 AS c, i * 4 AS d, i * 5 AS "
		                          "e FROM range(1000) t(i)"));
		for (idx_t i = 0; i < 9; i++) {
			auto suffix = std::to_string(i);
			REQUIRE_NO_FAIL(con.Query("CREATE TABLE candles_" + suffix + " AS FROM narrow"));
			REQUIRE_NO_FAIL(con.Query("CREATE TABLE other_" + suffix + " AS FROM wide"));
			REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		}
		for (idx_t i = 0; i < 9; i++) {
			REQUIRE_NO_FAIL(con.Query("DROP TABLE other_" + std::to_string(i)));
		}
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	DuckDB db(path, config.get());
	Connection con(db);
	auto result = con.Query("SELECT count(*) FROM duckdb_tables()");
	REQUIRE(CHECK_COLUMN(result, 0, {9}));
	REQUIRE(CountNextPointersToFreeBlocks(con, 9) == 0);
	DeleteDatabase(path);
}
