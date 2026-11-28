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
	//! Whether a SET changes the value of the setting, which has unset_value while it is not set (if known)
	bool SetChangesValue(idx_t setting_index, const Value &target_value, optional_ptr<const Value> unset_value) const;
	//! Whether a RESET changes the value of the setting
	bool ClearChangesValue(idx_t setting_index, optional_ptr<const Value> unset_value) const;

private:
	vector<GenericSetting> settings;
};

struct GlobalUserSettings;

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

	//! unset_value is the value that the setting has while it is not set here, the default (if known)
	void SetUserSetting(idx_t setting_index, Value target_value, optional_ptr<const Value> unset_value = nullptr);
	void ClearSetting(idx_t setting_index, optional_ptr<const Value> unset_value = nullptr);
	bool IsSet(idx_t setting_index) const;
	SettingLookupResult TryGetSetting(idx_t setting_index, Value &result_value) const;
	bool HasExtensionOption(const Identifier &name) const;
	idx_t AddExtensionOption(const Identifier &name, ExtensionOption extension_option);
	identifier_map_t<ExtensionOption> GetExtensionSettings() const;
	bool TryGetExtensionOption(const Identifier &name, ExtensionOption &result) const;
	//! The default value of the extension setting with the given index, if it has one
	bool TryGetExtensionDefault(idx_t setting_index, Value &result) const;
	optional_idx TryGetExtensionSettingIndex(const Identifier &name) const;
	hugeint_t GetUUID() const;
	//! The number of times the value of a database-global setting changed
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
	//! Incremented when the value of a setting changes, and not when an extension adds a setting
	atomic<idx_t> change_count;
	//! Settings uuid
	hugeint_t uuid;
};

struct LocalUserSettings {
public:
	~LocalUserSettings();

	//! unset_value is the value that the client reads while the setting is not set here (if known)
	void SetUserSetting(idx_t setting_index, Value target_value, optional_ptr<const Value> unset_value = nullptr);
	void ClearSetting(idx_t setting_index, optional_ptr<const Value> unset_value = nullptr);
	bool IsSet(idx_t setting_index) const;
	SettingLookupResult TryGetSetting(const GlobalUserSettings &global_settings, idx_t setting_index,
	                                  Value &result_value) const;
	//! The number of times the value of a setting of the client changed
	idx_t GetChangeCount() const;
	//! Counts the change of a setting that is not kept in this map
	void RegisterChange();

private:
	//! Client-local settings
	UserSettingsMap settings_map;
	//! Incremented when the value of a setting changes
	idx_t change_count = 0;
};
} // namespace duckdb
