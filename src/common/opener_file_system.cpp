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
	vector<string> search_paths;
	if (GetRestrictedSearchPaths(path, search_paths)) {
		return GlobInSearchPaths(path, search_paths);
	}
	VerifyCanAccessFile(path);
	return GetFileSystem().Glob(path, input, GetOpener());
}

bool OpenerFileSystem::GetRestrictedSearchPaths(const string &path, vector<string> &search_paths) {
	auto opener = GetOpener();
	if (!opener) {
		return false;
	}
	auto db = opener->TryGetDatabase();
	if (!db || Settings::Get<EnableExternalAccessSetting>(db->config)) {
		return false;
	}
	auto expanded_path = FileSystem::ExpandPath(path, opener);
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

unique_ptr<MultiFileList> OpenerFileSystem::GlobInSearchPaths(const string &path, const vector<string> &search_paths) {
	// the same order as LocalFileSystem: a name without glob is looked up in the working directory first
	auto &fs = GetFileSystem();
	auto opener = GetOpener();
	vector<string> candidates;
	auto has_glob = HasGlob(path);
	if (!has_glob) {
		candidates.push_back(path);
	}
	for (auto &search_path : search_paths) {
		candidates.push_back(fs.JoinPath(search_path, path));
	}
	bool any_allowed = false;
	vector<OpenFileInfo> files;
	for (idx_t i = 0; i < candidates.size(); i++) {
		// a location outside the allowed ones is not looked at, not even whether it exists
		if (!IsAccessAllowed(candidates[i], FileType::FILE_TYPE_REGULAR)) {
			continue;
		}
		any_allowed = true;
		// the checked form of the path, which is absolute, so that the search path is not applied again
		auto checked_path = fs.CanonicalizePath(FileSystem::ExpandPath(candidates[i], opener), opener);
		for (auto &file : fs.Glob(checked_path, FileGlobOptions::ALLOW_EMPTY, opener)->GetAllFiles()) {
			files.push_back(std::move(file));
		}
		if (!has_glob && i == 0 && !files.empty()) {
			break;
		}
	}
	if (!any_allowed) {
		throw CannotAccessError(path, FileType::FILE_TYPE_REGULAR);
	}
	return make_uniq<SimpleMultiFileList>(std::move(files));
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
