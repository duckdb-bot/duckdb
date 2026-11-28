//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/user_settings.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/setting_info.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {

struct GenericSetting {
	GenericSetting() : is_set(false) {
	}

	bool is_set;
	Value value;
};

struct UserSettingsMap {
public:
	//! Returns whether the setting was updated.
	bool SetUserSetting(idx_t setting_index, Value target_value);
	void ClearSetting(idx_t setting_index);
	bool IsSet(idx_t setting_index) const;
	bool TryGetSetting(idx_t setting_index, Value &result_value) const;
	//! The number of setting indexes that the map has room for
	idx_t SettingCount() const;

private:
	vector<GenericSetting> settings;
};

struct GlobalUserSettings;

//! The values of the settings that a client reads, without those that are at their default
struct SettingsSnapshot {
	//! Sorted by setting index
	vector<pair<idx_t, Value>> values;

	bool Equals(const SettingsSnapshot &other) const;
};

struct CachedGlobalSettings {
	CachedGlobalSettings();
	CachedGlobalSettings(const GlobalUserSettings &global_user_settings, idx_t version, UserSettingsMap settings,
	                     const identifier_map_t<ExtensionOption> &extension_parameters);

	optional_ptr<const GlobalUserSettings> global_user_settings;
	idx_t version;
	UserSettingsMap settings;
	identifier_map_t<idx_t> extension_setting_indexes;
	hugeint_t uuid;
};

//! The settings cache of a worker thread. While it is initialized, the thread reads settings through it instead of
//! through the thread_local cache, whose first use registers a destructor that glibc cannot fail to allocate without
//! ending the process
class WorkerSettingsCache {
public:
	WorkerSettingsCache();
	~WorkerSettingsCache();
	WorkerSettingsCache(const WorkerSettingsCache &) = delete;
	WorkerSettingsCache &operator=(const WorkerSettingsCache &) = delete;
	WorkerSettingsCache(WorkerSettingsCache &&) = delete;
	WorkerSettingsCache &operator=(WorkerSettingsCache &&) = delete;

public:
	bool IsInitialized() const;
	//! Create the cache, this allocates
	void Initialize();

private:
	unique_ptr<CachedGlobalSettings> cache;
	//! The cache the thread read through before, restored when this one goes away
	optional_ptr<CachedGlobalSettings> previous;
};

struct GlobalUserSettings {
public:
	GlobalUserSettings();
	// enable copy constructors
	GlobalUserSettings(const GlobalUserSettings &other);
	GlobalUserSettings &operator=(const GlobalUserSettings &);

	void SetUserSetting(idx_t setting_index, Value target_value);
	void ClearSetting(idx_t setting_index);
	bool IsSet(idx_t setting_index) const;
	SettingLookupResult TryGetSetting(idx_t setting_index, Value &result_value) const;
	bool HasExtensionOption(const Identifier &name) const;
	idx_t AddExtensionOption(const Identifier &name, ExtensionOption extension_option);
	identifier_map_t<ExtensionOption> GetExtensionSettings() const;
	bool TryGetExtensionOption(const Identifier &name, ExtensionOption &result) const;
	optional_idx TryGetExtensionSettingIndex(const Identifier &name) const;
	//! The default value of the extension setting with the given index, if it has one
	bool TryGetExtensionDefault(idx_t setting_index, Value &result) const;
	hugeint_t GetUUID() const;
	//! The version of the settings, which moves whenever a setting is stored, removed or added
	idx_t GetVersion() const;
	//! Copies the settings and returns their version
	idx_t CopySettings(UserSettingsMap &result) const;
	//! The number of changes of database-global settings that are not kept in this map
	idx_t GetChangeCount() const;
	//! Counts the change of a setting that is not kept in this map
	void RegisterChange();

#ifndef __MINGW32__
	CachedGlobalSettings &GetSettings() const;
#endif

private:
	mutable mutex lock;
	//! Database-global settings
	UserSettingsMap settings_map;
	//! Extra parameters that can be SET for loaded extensions
	identifier_map_t<ExtensionOption> extension_parameters;
	//! Current version of the settings - incremented when settings are modified
	atomic<idx_t> settings_version;
	//! Incremented when a setting that is not kept in the map changes
	atomic<idx_t> change_count;
	//! Settings uuid
	hugeint_t uuid;
};

struct LocalUserSettings {
public:
	~LocalUserSettings();

	void SetUserSetting(idx_t setting_index, Value target_value);
	void ClearSetting(idx_t setting_index);
	bool IsSet(idx_t setting_index) const;
	SettingLookupResult TryGetSetting(const GlobalUserSettings &global_settings, idx_t setting_index,
	                                  Value &result_value) const;
	//! The values of the settings that the client reads now. It is the same object as long as no value changes
	const shared_ptr<const SettingsSnapshot> &GetSnapshot(const DBConfig &config);
	//! The number of changes of settings of the client that are not kept in this map
	idx_t GetChangeCount() const;
	//! Counts the change of a setting that is not kept in this map
	void RegisterChange();

private:
	//! Client-local settings
	UserSettingsMap settings_map;
	//! Incremented when a setting is stored or removed
	idx_t version = 0;
	//! Incremented when a setting that is not kept in the map changes
	idx_t change_count = 0;
	//! The snapshot of the settings at the versions below
	shared_ptr<const SettingsSnapshot> snapshot;
	idx_t snapshot_version = 0;
	idx_t snapshot_global_version = 0;
};
} // namespace duckdb
