//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/database_file_path_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "duckdb/common/reference_map.hpp"

namespace duckdb {
struct AttachInfo;
struct AttachOptions;
class DatabaseManager;

enum class InsertDatabasePathResult { SUCCESS, ALREADY_EXISTS };

struct DatabasePathInfo {
	DatabasePathInfo(DatabaseManager &manager, const Identifier &name_p, AccessMode access_mode);

	string name;
	AccessMode access_mode;
	reference_set_t<DatabaseManager> attached_databases;
	idx_t reference_count = 1;
};

//! The DatabaseFilePathManager is used to ensure we only ever open a single database file once
class DatabaseFilePathManager {
public:
	idx_t ApproxDatabaseCount() const;
	InsertDatabasePathResult InsertDatabasePath(DatabaseManager &manager, const string &path, const Identifier &name,
	                                            OnCreateConflict on_conflict, AttachOptions &options);
	//! Erase a database path - indicating we are done with using it
	void EraseDatabasePath(const string &path);
	//! Called when a database is detached, but before it is fully finished being used
	void DetachDatabase(DatabaseManager &manager, const string &path);

private:
	//! The lock to add entries to the db_paths map
	mutable mutex db_paths_lock;
	//! A set containing all attached database path
	//! This allows to attach many databases efficiently, and to avoid attaching the
	//! same file path twice
#if defined(_WIN32) || defined(__APPLE__)
	case_insensitive_map_t<DatabasePathInfo> db_paths;
#else  // !(_WIN32 or __APPLE__)
	unordered_map<string, DatabasePathInfo> db_paths;
#endif // _WIN32 or __APPLE__
};

//! The DuckDB files that the database instances of this process have open. Each instance keeps its own header, free
//! list and WAL of a file, and the lock of the file belongs to the process, so no two instances may open the same
//! file while one of them writes to it
class DatabaseFileRegistry {
public:
	//! The registry of the process
	static shared_ptr<DatabaseFileRegistry> Get();

	//! Registers an open of the file by the database manager. Returns false while another instance is still reading
	//! the type of the file, and throws if another instance has it open and one of the two writes to it
	bool TryRegister(DatabaseManager &manager, const string &path, const Identifier &name, AccessMode access_mode);
	//! Called once the file at the path is known to be a DuckDB database
	void Confirm(const string &path);
	//! Unregisters an open of the file by the database manager
	void Unregister(DatabaseManager &manager, const string &path, AccessMode access_mode);

private:
	struct OpenFile {
		//! The number of opens of the file by each database manager
		reference_map_t<DatabaseManager, idx_t> opens;
		//! The number of opens that write to the file
		idx_t writer_count = 0;
		//! Whether the file was found to be a DuckDB database, until then it may still be released
		bool is_database_file = false;
	};

	mutex files_lock;
	//! Compared with case, as the volume of a path may be case sensitive
	unordered_map<string, OpenFile> files;
};

} // namespace duckdb
