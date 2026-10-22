#include "duckdb/common/opener_file_system.hpp"
#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/path.hpp"

namespace duckdb {

void OpenerFileSystem::RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) {
	GetFileSystem().RegisterCompressionFilesystem(std::move(fs));
}

unique_ptr<MemoryMappedFile> OpenerFileSystem::MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
                                                             const MMapOptions &options,
                                                             optional_ptr<FileOpener> opener) {
	VerifyNoOpener(opener);
	VerifyCanAccessFile(path.path);
	if (IsDuckDBExtensionName(path.path)) {
		VerifyCanAccessExtension(path.path, flags);
	}
	return GetFileSystem().MemoryMapFile(path, flags, options, GetOpener());
}

static PermissionException CannotAccessError(const string &path, FileType type) {
	return PermissionException("Cannot access %s \"%s\" - file system operations are disabled by configuration",
	                           type == FileType::FILE_TYPE_DIR ? "directory" : "file", path);
}

unique_ptr<MultiFileList> OpenerFileSystem::GlobFilesExtended(const string &path, const FileGlobInput &input,
                                                              optional_ptr<FileOpener> opener) {
	VerifyNoOpener(opener);
	string expanded_path;
	vector<string> search_paths;
	if (!GetRestrictedSearchPaths(path, expanded_path, search_paths)) {
		VerifyCanAccessFile(path);
		return GetFileSystem().Glob(path, input, GetOpener());
	}
	// the path is checked where file_search_path resolves it, and only the allowed locations are looked at; a glob is
	// expanded in the search paths only, a name without glob is also looked up in the working directory
	auto &fs = GetFileSystem();
	bool any_allowed = !HasGlob(expanded_path) && IsAccessAllowed(expanded_path, FileType::FILE_TYPE_REGULAR);
	for (auto &search_path : search_paths) {
		any_allowed =
		    any_allowed || IsAccessAllowed(fs.JoinPath(search_path, expanded_path), FileType::FILE_TYPE_REGULAR);
	}
	if (!any_allowed) {
		throw CannotAccessError(path, FileType::FILE_TYPE_REGULAR);
	}
	FileGlobInput restricted_input = input;
	restricted_input.can_access_location = [this](const string &location) {
		return IsAccessAllowed(location, FileType::FILE_TYPE_REGULAR);
	};
	return fs.Glob(path, restricted_input, GetOpener());
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
	if (IsRemoteFile(expanded_path) || Path::FromString(expanded_path).IsAbsolute()) {
		return false;
	}
	Value search_path_setting;
	if (!opener->TryGetCurrentSetting("file_search_path", search_path_setting)) {
		return false;
	}
	search_paths = StringUtil::Split(search_path_setting.ToString(), ',');
	return !search_paths.empty();
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
