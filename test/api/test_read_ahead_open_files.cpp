#include "catch.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "test_helpers.hpp"

#if defined(__linux__) && !defined(DUCKDB_NO_THREADS)
#include <dirent.h>
#include <sys/resource.h>
#endif

using namespace duckdb;

#if defined(__linux__) && !defined(DUCKDB_NO_THREADS)
static idx_t CountOpenFileDescriptors() {
	idx_t count = 0;
	auto dir = opendir("/proc/self/fd");
	if (!dir) {
		return 0;
	}
	while (readdir(dir)) {
		count++;
	}
	closedir(dir);
	return count;
}
#endif

TEST_CASE("Read-ahead keeps a scan of many files within the file descriptor limit", "[api][.]") {
#if defined(__linux__) && !defined(DUCKDB_NO_THREADS)
	DuckDB db(nullptr);
	Connection con(db);
	auto dir = TestCreatePath("read_ahead_open_files");
	REQUIRE_NO_FAIL(con.Query("SET partitioned_write_max_open_files = 100"));
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT range AS i, range AS p FROM range(3000)) TO '" + dir +
	                          "' (FORMAT parquet, PARTITION_BY p)"));
	struct rlimit original;
	REQUIRE(getrlimit(RLIMIT_NOFILE, &original) == 0);
	const rlim_t limit = CountOpenFileDescriptors() + 240;
	if (original.rlim_cur < limit) {
		SKIP_TEST("the file descriptor limit is too low");
		return;
	}
	// a window of 512 files without the limit, more scan threads, and a deep queue of jobs
	const vector<vector<string>> settings {{"SET threads = 8", "SET async_threads = 256"},
	                                       {"SET threads = 64", "SET async_threads = 32"},
	                                       {"SET threads = 4", "SET async_threads = 32", "SET read_ahead_depth = 256"}};
	for (auto &statements : settings) {
		for (auto &statement : statements) {
			REQUIRE_NO_FAIL(con.Query(statement));
		}
		struct rlimit limited = original;
		limited.rlim_cur = limit;
		REQUIRE(setrlimit(RLIMIT_NOFILE, &limited) == 0);
		auto result = con.Query("SELECT count(*) FROM read_parquet('" + dir + "/*/*.parquet')");
		REQUIRE(setrlimit(RLIMIT_NOFILE, &original) == 0);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {3000}));
	}
#else
	SKIP_TEST("needs Linux and threads");
#endif
}

namespace {

class OpenAheadFileSystem;

//! Counts its file as open from its creation to its destruction
class CountedFileHandle : public FileHandle {
public:
	CountedFileHandle(OpenAheadFileSystem &fs, unique_ptr<FileHandle> inner_p);
	~CountedFileHandle() override;

	void Close() override {
		inner->Close();
	}

	unique_ptr<FileHandle> inner;

private:
	OpenAheadFileSystem &counting_fs;
};

//! Serves read-ahead-count://<path> from a local directory, and records the most files it had open at once
class OpenAheadFileSystem : public FileSystem {
public:
	OpenAheadFileSystem() : local_prefix(TestCreatePath("read_ahead_count_")) {
	}

	static const string &Prefix() {
		static const string prefix = "read-ahead-count://";
		return prefix;
	}

	string MapPath(const string &path) const {
		if (!StringUtil::StartsWith(path, Prefix())) {
			return path;
		}
		return local_prefix + path.substr(Prefix().size());
	}

	string GetName() const override {
		return "OpenAheadFileSystem";
	}

	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, Prefix());
	}

	bool IsLocalFileSystem() const override {
		return true;
	}

	bool CanSeek() override {
		return true;
	}

	bool FileExists(const string &path, optional_ptr<FileOpener> opener) override {
		return local_fs.FileExists(MapPath(path), opener);
	}

	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override {
		vector<OpenFileInfo> result;
		if (local_fs.FileExists(MapPath(path), opener)) {
			result.emplace_back(path);
		}
		return result;
	}

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override {
		return make_uniq<CountedFileHandle>(*this, local_fs.OpenFile(MapPath(path), flags, opener));
	}

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		local_fs.Read(Inner(handle), buffer, nr_bytes, location);
	}
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		return local_fs.Read(Inner(handle), buffer, nr_bytes);
	}
	int64_t GetFileSize(FileHandle &handle) override {
		return local_fs.GetFileSize(Inner(handle));
	}
	timestamp_t GetLastModifiedTime(FileHandle &handle) override {
		return local_fs.GetLastModifiedTime(Inner(handle));
	}
	string GetVersionTag(FileHandle &handle) override {
		return local_fs.GetVersionTag(Inner(handle));
	}
	FileType GetFileType(FileHandle &handle) override {
		return local_fs.GetFileType(Inner(handle));
	}
	FileMetadata Stats(FileHandle &handle) override {
		return local_fs.Stats(Inner(handle));
	}
	void Seek(FileHandle &handle, idx_t location) override {
		local_fs.Seek(Inner(handle), location);
	}
	idx_t SeekPosition(FileHandle &handle) override {
		return local_fs.SeekPosition(Inner(handle));
	}
	bool OnDiskFile(FileHandle &handle) override {
		return local_fs.OnDiskFile(Inner(handle));
	}

	void Opened(const string &path) {
		lock_guard<mutex> guard(lock);
		open_handles[path]++;
		max_open_files = MaxValue<idx_t>(max_open_files, open_handles.size());
	}
	void Closed(const string &path) {
		lock_guard<mutex> guard(lock);
		if (--open_handles[path] == 0) {
			open_handles.erase(path);
		}
	}
	idx_t MaxOpenFiles() {
		lock_guard<mutex> guard(lock);
		return max_open_files;
	}

private:
	static FileHandle &Inner(FileHandle &handle) {
		return *handle.Cast<CountedFileHandle>().inner;
	}

	LocalFileSystem local_fs;
	const string local_prefix;
	mutex lock;
	unordered_map<string, idx_t> open_handles;
	idx_t max_open_files = 0;
};

CountedFileHandle::CountedFileHandle(OpenAheadFileSystem &fs, unique_ptr<FileHandle> inner_p)
    : FileHandle(fs, inner_p->path, inner_p->flags), inner(std::move(inner_p)), counting_fs(fs) {
	counting_fs.Opened(path);
}

CountedFileHandle::~CountedFileHandle() {
	counting_fs.Closed(path);
}

} // namespace

TEST_CASE("Read-ahead keeps the files open ahead of a scan within its window", "[api]") {
#ifdef DUCKDB_NO_THREADS
	SKIP_TEST("read-ahead opens files on the async pool");
	return;
#else
	DuckDB db(nullptr);
	Connection con(db);
	auto fs = make_uniq<OpenAheadFileSystem>();
	auto dir = fs->MapPath(OpenAheadFileSystem::Prefix() + "files");
	REQUIRE_NO_FAIL(con.Query("SET partitioned_write_max_open_files = 100"));
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT range AS i, range AS p FROM range(1000)) TO '" + dir +
	                          "' (FORMAT parquet, PARTITION_BY p)"));
	string file_list;
	for (auto &file : LocalFileSystem().GlobFiles(dir + "/*/*.parquet")) {
		auto path = OpenAheadFileSystem::Prefix() + "files" + file.path.substr(dir.size());
		file_list += (file_list.empty() ? "'" : ", '") + path + "'";
	}
	auto &counting_fs = *fs;
	FileSystem::GetFileSystem(*con.context).RegisterSubSystem(std::move(fs));

	// many scan threads produce jobs for a small window
	const idx_t threads = 32;
	const idx_t async_threads = 4;
	REQUIRE_NO_FAIL(con.Query("SET threads = " + to_string(threads)));
	REQUIRE_NO_FAIL(con.Query("SET async_threads = " + to_string(async_threads)));
	auto result = con.Query("SELECT count(*), sum(i) FROM read_parquet([" + file_list + "])");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {1000}));
	REQUIRE(CHECK_COLUMN(result, 1, {499500}));
	// the window of files opened ahead and files of queued jobs, the file of the job each thread decodes and of the
	// next one it claims, and the file the bind opened
	const idx_t open_window = 2 * async_threads;
	REQUIRE(counting_fs.MaxOpenFiles() <= open_window + 2 * threads + 1);
#endif
}
