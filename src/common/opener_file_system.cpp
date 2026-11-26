#include "duckdb/common/opener_file_system.hpp"
#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system_wrapper.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/path.hpp"

namespace duckdb {

unique_ptr<MemoryMappedFile> OpenerFileSystem::MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
                                                             const MMapOptions &options,
                                                             optional_ptr<FileOpener> opener) {
	VerifyNoOpener(opener);
	VerifyCanAccessFile(path.path);
	if (IsDuckDBExtensionName(path.path)) {
		VerifyCanAccessExtension(path.path, flags);
	}
	return FindFileSystem(path.path).MemoryMapFile(path, flags, options, GetOpener());
}

static PermissionException CannotAccessError(const string &path, FileType type) {
	return PermissionException("Cannot access %s \"%s\" - file system operations are disabled by configuration",
	                           type == FileType::FILE_TYPE_DIR ? "directory" : "file", path);
}

vector<OpenFileInfo> OpenerFileSystem::Glob(const string &path, FileOpener *opener) {
	VerifyNoOpener(opener);
	return GlobFilesExtended(path, FileGlobOptions::ALLOW_EMPTY, nullptr)->GetAllFiles();
}

unique_ptr<MultiFileList> OpenerFileSystem::GlobFilesExtended(const string &path, const FileGlobInput &input,
                                                              optional_ptr<FileOpener> opener) {
	VerifyNoOpener(opener);
	string expanded_path;
	vector<string> search_paths;
	if (!GetRestrictedSearchPaths(path, expanded_path, search_paths)) {
		VerifyCanAccessFile(path);
		return FindFileSystem(path).Glob(path, input, GetOpener());
	}
	// the path is checked where file_search_path resolves it, and only the allowed locations are looked at
	// (a glob only in the search paths, a name without glob also in the working directory)
	// a remote location is never searched, so it is refused without looking up its file system
	auto can_access_location = [this](const string &location) {
		return !IsRemoteFile(location) && IsAccessAllowed(location, FileType::FILE_TYPE_REGULAR);
	};
	bool any_allowed = !HasGlob(expanded_path) && can_access_location(expanded_path);
	for (idx_t i = 0; i < search_paths.size() && !any_allowed; i++) {
		auto location = JoinPath(FileSystem::ExpandPath(search_paths[i], GetOpener()), expanded_path);
		any_allowed = can_access_location(location);
	}
	if (!any_allowed) {
		throw CannotAccessError(path, FileType::FILE_TYPE_REGULAR);
	}
	FileGlobInput restricted_input = input;
	restricted_input.can_access_location = can_access_location;
	return FindFileSystem(path).Glob(path, restricted_input, GetOpener());
}

bool OpenerFileSystem::GetRestrictedSearchPaths(const string &path, string &expanded_path,
                                                vector<string> &search_paths) {
	auto opener = GetOpener();
	if (!opener) {
		return false;
	}
	auto db = opener->TryGetDatabase();
	if (!db || Settings::Get<EnableExternalAccessSetting>(db->config)) {
		return false;
	}
	expanded_path = FileSystem::ExpandPath(path, opener);
	if (IsRemoteFile(expanded_path)) {
		return false;
	}
	auto parsed_path = Path::FromString(expanded_path);
	if (parsed_path.IsAbsolute()) {
		return false;
	}
	// a relative path that normalization starts with a "~" segment is refused, since the glob would expand it as the
	// home directory where the check did not
	auto &segments = parsed_path.GetPathSegments();
	if (parsed_path.GetBase().empty() && !segments.empty() && segments[0] == "~") {
		throw CannotAccessError(path, FileType::FILE_TYPE_REGULAR);
	}
	Value search_path_setting;
	if (!opener->TryGetCurrentSetting("file_search_path", search_path_setting)) {
		return false;
	}
	search_paths = StringUtil::Split(search_path_setting.ToString(), ',');
	return !search_paths.empty();
}

unique_ptr<FileHandle> OpenerFileSystem::OpenCachedFile(const OpenFileInfo &file, const FileOpenFlags &flags) {
	// the cache holds the file as stored, the file system applies the compression on top of it
	auto raw_flags = flags;
	raw_flags.SetCompression(FileCompressionType::UNCOMPRESSED);
	raw_flags.SetCachingMode(CachingMode::NO_CACHING);

	// the caching file system is kept alive by the handles it hands out
	auto caching_file_system =
	    make_shared_ptr<CachingFileSystemWrapper>(FindFileSystem(file.path), GetOpener(), flags.GetCachingMode());
	auto handle = caching_file_system->OpenFile(file, raw_flags, GetOpener());
	if (!handle) {
		return nullptr;
	}
	return GetFileSystem().WrapOpenedFile(std::move(handle), file, flags, GetOpener());
}

void OpenerFileSystem::VerifyNoOpener(optional_ptr<FileOpener> opener) {
	if (opener) {
		throw InternalException("OpenerFileSystem cannot take an opener - the opener is pushed automatically");
	}
}
bool OpenerFileSystem::IsAccessAllowed(const string &path, FileType type) {
	auto opener = GetOpener();
	if (!opener) {
		return true;
	}
	auto db = opener->TryGetDatabase();
	if (!db) {
		return true;
	}
	return db->config.CanAccessFile(path, type, opener);
}

void OpenerFileSystem::VerifyCanAccessFileInternal(const string &path, FileType type) {
	if (!IsAccessAllowed(path, type)) {
		throw CannotAccessError(path, type);
	}
}

void OpenerFileSystem::VerifyCanAccessFile(const string &path) {
	VerifyCanAccessFileInternal(path, FileType::FILE_TYPE_REGULAR);
}

void OpenerFileSystem::VerifyCanAccessDirectory(const string &path) {
	VerifyCanAccessFileInternal(path, FileType::FILE_TYPE_DIR);
}

} // namespace duckdb
