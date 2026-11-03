//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/data_table_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/storage_lock.hpp"
#include "duckdb/storage/table/table_index_list.hpp"

namespace duckdb {
class AttachedDatabase;
class DatabaseInstance;
struct CheckpointOptions;
class TableIOManager;
class RowGroupCollection;
class IndexBuildRegistration;

struct DataTableInfo {
	friend class DataTable;
	friend class IndexBuildRegistration;

public:
	DataTableInfo(AttachedDatabase &db, shared_ptr<TableIOManager> table_io_manager_p, vector<Identifier> schema_path,
	              Identifier table);

	//! Bind unknown indexes throwing an exception if binding fails.
	//! Only binds the specified index type, or all if no type is specified.
	void BindIndexes(ClientContext &context, const optional<string> &index_type = {});

	//! Whether or not the table is temporary
	bool IsTemporary() const;

	AttachedDatabase &GetDB() const {
		return db;
	}

	TableIOManager &GetIOManager() {
		return *table_io_manager;
	}

	TableIndexList &GetIndexes() {
		return indexes;
	}
	//! Find and move out an IndexStorageInfo by name from the stored collection.
	IndexStorageInfo ExtractIndexStorageInfo(const Identifier &name);
	bool AppendRequiresNewRowGroup(RowGroupCollection &collection, optional_idx checkpoint_id);
	optional_idx CheckpointRowGroupCount(const CheckpointOptions &options) const;
	void VerifyIndexBuffers() const;

	//! Throws if an index is built from or was created on one of the columns, as an in-place update would not reach it.
	//! The caller holds the lock of GetInPlaceUpdateLock
	void VerifyInPlaceUpdate(const vector<PhysicalIndex> &column_ids);
	//! Held by in-place updates while they write
	unique_ptr<StorageLockKey> GetInPlaceUpdateLock();

	Identifier GetSchemaName();
	//! The full (possibly nested) schema path of the table
	const vector<Identifier> &GetSchemaPath() const;
	Identifier GetTableName();
	void SetTableName(Identifier name);

private:
	//! The database instance of the table
	AttachedDatabase &db;
	//! The table IO manager
	shared_ptr<TableIOManager> table_io_manager;
	//! Lock for modifying the name
	mutex name_lock;
	//! The (possibly nested) schema path of the table, outermost schema first
	vector<Identifier> schema_path;
	//! The name of the table
	Identifier table;
	//! The physical list of indexes of this table
	TableIndexList indexes;
	//! Index storage information of the indexes created by this table
	vector<IndexStorageInfo> index_storage_infos;
	//! The last seen checkpoint while doing a concurrent operation, if any
	optional_idx last_seen_checkpoint;
	//! The amount of row groups the checkpoint is processing
	optional_idx checkpoint_row_group_count;
	//! Shared by in-place updates while they write, and exclusive while an index build is registered
	StorageLock in_place_update_lock;
	//! Lock for the index builds
	mutex index_builds_lock;
	//! The index builds that read the table
	vector<const_reference<IndexBuildRegistration>> index_builds;
};

//! Refuses in-place updates of the columns an index is built from, from before the build reads them until it ends
class IndexBuildRegistration {
public:
	//! Waits for the in-place updates that are writing
	IndexBuildRegistration(shared_ptr<DataTableInfo> info, vector<column_t> columns);
	~IndexBuildRegistration();

	//! The physical columns the build reads
	const vector<column_t> &GetColumns() const {
		return columns;
	}

private:
	shared_ptr<DataTableInfo> info;
	vector<column_t> columns;
};

} // namespace duckdb
