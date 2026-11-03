#include "duckdb/main/database_file_path_manager.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"

namespace duckdb {

DatabasePathInfo::DatabasePathInfo(DatabaseManager &manager, const Identifier &name_p, AccessMode access_mode)
    : access_mode(access_mode) {
	attached_databases[manager].push_back(name_p.GetIdentifierName());
}

idx_t DatabaseFilePathManager::ApproxDatabaseCount() const {
	lock_guard<mutex> path_lock(db_paths_lock);
	return db_paths.size();
}

InsertDatabasePathResult DatabaseFilePathManager::InsertDatabasePath(DatabaseManager &manager, const string &path,
                                                                     const Identifier &name,
                                                                     OnCreateConflict on_conflict,
                                                                     AttachOptions &options) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		throw InternalException("DatabaseFilePathManager::InsertDatabasePath - cannot insert in-memory database");
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.emplace(path, DatabasePathInfo(manager, name, options.access_mode));
	if (!entry.second) {
		auto &existing = entry.first->second;
		auto attached = existing.attached_databases.find(manager);
		bool attached_in_this_system = false;
		if (attached != existing.attached_databases.end()) {
			auto &names = attached->second;
			attached_in_this_system = std::find(names.begin(), names.end(), name) != names.end();
		}
		if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && attached_in_this_system) {
			return InsertDatabasePathResult::ALREADY_EXISTS;
		}
		if (options.access_mode == AccessMode::READ_ONLY && existing.access_mode == AccessMode::READ_ONLY) {
			// all attaches are in read-only mode - there is no conflict, just increase the reference count
			existing.attached_databases[manager].push_back(name.GetIdentifierName());
			existing.reference_count++;
		} else {
			if (existing.attached_databases.empty() && on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT &&
			    existing.detached_name == name) {
				throw ResourceInUseException(
				    "Unique file handle conflict: Cannot attach %s - the database file \"%s\" is in "
				    "the process of being detached",
				    name, path);
			}
			// name a database of this system if there is one
			string holder_name = existing.detached_name;
			if (attached != existing.attached_databases.end()) {
				holder_name = attached->second[0];
			} else if (!existing.attached_databases.empty()) {
				holder_name = existing.attached_databases.begin()->second[0];
			}
			throw ResourceInUseException(
			    "Unique file handle conflict: Cannot attach %s - the database file \"%s\" is already "
			    "attached by database \"%s\"",
			    name, path, holder_name);
		}
	}
	options.stored_database_path = make_uniq<StoredDatabasePath>(manager, *this, path, name);
	return InsertDatabasePathResult::SUCCESS;
}

bool DatabaseFilePathManager::IsDatabasePath(const string &path) const {
	lock_guard<mutex> path_lock(db_paths_lock);
	return db_paths.find(path) != db_paths.end();
}

void DatabaseFilePathManager::EraseDatabasePath(const string &path) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		return;
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry != db_paths.end()) {
		if (entry->second.reference_count <= 1) {
			db_paths.erase(entry);
		} else {
			entry->second.reference_count--;
		}
	}
}

void DatabaseFilePathManager::DetachDatabase(DatabaseManager &manager, const string &path, const string &name) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		return;
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry == db_paths.end()) {
		return;
	}
	auto attached = entry->second.attached_databases.find(manager);
	if (attached == entry->second.attached_databases.end()) {
		return;
	}
	auto &names = attached->second;
	auto position = std::find(names.begin(), names.end(), name);
	if (position != names.end()) {
		names.erase(position);
		entry->second.detached_name = name;
	}
	if (names.empty()) {
		entry->second.attached_databases.erase(attached);
	}
}

void DatabaseFilePathManager::RenameDatabase(DatabaseManager &manager, const string &path, const string &old_name,
                                             const string &new_name) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		return;
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry == db_paths.end()) {
		return;
	}
	auto attached = entry->second.attached_databases.find(manager);
	if (attached == entry->second.attached_databases.end()) {
		return;
	}
	auto position = std::find(attached->second.begin(), attached->second.end(), old_name);
	if (position != attached->second.end()) {
		*position = new_name;
	}
}

} // namespace duckdb
