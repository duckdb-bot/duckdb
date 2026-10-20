#include "duckdb/main/database_file_path_manager.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"

namespace duckdb {

DatabasePathInfo::DatabasePathInfo(DatabaseManager &manager, const Identifier &name_p, AccessMode access_mode)
    : name(name_p.GetIdentifierName()), access_mode(access_mode) {
	attached_databases[manager].push_back(name);
}

static bool HasName(const vector<string> &names, const Identifier &name) {
	for (auto &entry : names) {
		if (entry == name) {
			return true;
		}
	}
	return false;
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
		// this system compares to the names it uses for the path, which a rename may have changed
		auto attached = existing.attached_databases.find(manager);
		vector<string> no_names;
		auto &names = attached == existing.attached_databases.end() ? no_names : attached->second;
		bool attached_in_this_system = HasName(names, name);
		bool already_exists = on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT &&
		                      (attached_in_this_system || (names.empty() && existing.name == name));
		if (options.access_mode == AccessMode::READ_ONLY && existing.access_mode == AccessMode::READ_ONLY) {
			if (already_exists && attached_in_this_system) {
				return InsertDatabasePathResult::ALREADY_EXISTS;
			}
			// all attaches are in read-only mode - there is no conflict, just increase the reference count
			existing.attached_databases[manager].push_back(name.GetIdentifierName());
			existing.reference_count++;
		} else {
			if (already_exists) {
				if (attached_in_this_system) {
					return InsertDatabasePathResult::ALREADY_EXISTS;
				}
				throw ResourceInUseException(
				    "Unique file handle conflict: Cannot attach %s - the database file \"%s\" is in "
				    "the process of being detached",
				    name, path);
			}
			throw ResourceInUseException(
			    "Unique file handle conflict: Cannot attach %s - the database file \"%s\" is already "
			    "attached by database \"%s\"",
			    name, path, names.empty() ? existing.name : names[0]);
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
	auto &info = entry->second;
	auto attached = info.attached_databases.find(manager);
	if (attached != info.attached_databases.end()) {
		auto position = std::find(attached->second.begin(), attached->second.end(), old_name);
		if (position != attached->second.end()) {
			*position = new_name;
		}
	}
	if (info.name == old_name) {
		info.name = new_name;
	}
}

} // namespace duckdb
