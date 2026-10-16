#include "duckdb/main/database_file_path_manager.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"

namespace duckdb {

DatabasePathInfo::DatabasePathInfo(DatabaseManager &manager, const Identifier &name_p, AccessMode access_mode)
    : name(name_p.GetIdentifierName()), access_mode(access_mode) {
	attached_databases.insert(manager);
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
		bool already_exists = false;
		bool attached_in_this_system = false;
		if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && existing.name == name) {
			already_exists = true;
			attached_in_this_system = existing.attached_databases.find(manager) != existing.attached_databases.end();
		}
		if (options.access_mode == AccessMode::READ_ONLY && existing.access_mode == AccessMode::READ_ONLY) {
			if (attached_in_this_system) {
				return InsertDatabasePathResult::ALREADY_EXISTS;
			}
			// all attaches are in read-only mode - there is no conflict, just increase the reference count
			existing.attached_databases.insert(manager);
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
			    name, path, existing.name);
		}
	}
	options.stored_database_path = make_uniq<StoredDatabasePath>(manager, *this, path, name);
	return InsertDatabasePathResult::SUCCESS;
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

void DatabaseFilePathManager::DetachDatabase(DatabaseManager &manager, const string &path) {
	if (path.empty() || path == IN_MEMORY_PATH) {
		return;
	}
	lock_guard<mutex> path_lock(db_paths_lock);
	auto entry = db_paths.find(path);
	if (entry != db_paths.end()) {
		entry->second.attached_databases.erase(manager);
	}
}

shared_ptr<DatabaseFileRegistry> DatabaseFileRegistry::Get() {
	static auto registry = make_shared_ptr<DatabaseFileRegistry>();
	return registry;
}

bool DatabaseFileRegistry::TryRegister(DatabaseManager &manager, const string &path, const Identifier &name,
                                       AccessMode access_mode) {
	bool writes = access_mode != AccessMode::READ_ONLY;
	lock_guard<mutex> guard(files_lock);
	auto &file = files[path];
	bool opened_elsewhere = false;
	for (auto &open : file.opens) {
		if (&open.first.get() != &manager) {
			opened_elsewhere = true;
			break;
		}
	}
	if (opened_elsewhere && (writes || file.writer_count > 0)) {
		if (!file.is_database_file) {
			return false;
		}
		throw ResourceInUseException("Unique file handle conflict: Cannot attach %s - the database file \"%s\" is "
		                             "already open in another database instance of this process",
		                             name, path);
	}
	file.opens[manager]++;
	if (writes) {
		file.writer_count++;
	}
	return true;
}

void DatabaseFileRegistry::Confirm(const string &path) {
	lock_guard<mutex> guard(files_lock);
	auto entry = files.find(path);
	if (entry != files.end()) {
		entry->second.is_database_file = true;
	}
}

void DatabaseFileRegistry::Unregister(DatabaseManager &manager, const string &path, AccessMode access_mode) {
	lock_guard<mutex> guard(files_lock);
	auto entry = files.find(path);
	if (entry == files.end()) {
		return;
	}
	if (access_mode != AccessMode::READ_ONLY) {
		entry->second.writer_count--;
	}
	auto &opens = entry->second.opens;
	auto open = opens.find(manager);
	if (open != opens.end() && --open->second == 0) {
		opens.erase(open);
	}
	if (opens.empty()) {
		files.erase(entry);
	}
}

} // namespace duckdb
