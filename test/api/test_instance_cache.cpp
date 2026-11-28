#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/db_instance_cache.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/local_file_system.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <thread>

using namespace duckdb;

static void background_thread_connect(DBInstanceCache *instance_cache, std::string *path) {
	try {
		DBConfig config;
		auto connection = instance_cache->GetOrCreateInstance(*path, config, true);
		connection.reset();
	} catch (std::exception &ex) {
		FAIL(ex.what());
	}
}

TEST_CASE("Test parallel connection and destruction of connections with database instance cache", "[api][.]") {
	DBInstanceCache instance_cache;

	for (idx_t i = 0; i < 100; i++) {
		auto path = TestCreatePath("instance_cache_parallel.db");

		DBConfig config;
		auto shared_db = instance_cache.GetOrCreateInstance(path, config, true);

		std::thread background_thread(background_thread_connect, &instance_cache, &path);
		shared_db.reset();
		background_thread.join();
		TestDeleteFile(path);
		REQUIRE(1);
	}
}
struct DelayingStorageExtension : StorageExtension {
	DelayingStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			std::this_thread::sleep_for(std::chrono::seconds(5));
			return make_uniq_base<Catalog, DuckCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

TEST_CASE("Test db creation does not block instance cache", "[api][.]") {
	DBInstanceCache instance_cache;
	using namespace std::chrono;

	auto second_creation_was_quick = false;
	shared_ptr<DuckDB> stick_around;
	std::thread t1 {[&instance_cache, &second_creation_was_quick, &stick_around]() {
		DBConfig db_config;

		StorageExtension::Register(db_config, "delay", make_shared_ptr<DelayingStorageExtension>());
		stick_around = instance_cache.GetOrCreateInstance("delay::memory:", db_config, true);

		const auto start_time = steady_clock::now();
		for (idx_t i = 0; i < 10; i++) {
			StorageExtension::Register(db_config, "delay", make_shared_ptr<DelayingStorageExtension>());
			instance_cache.GetOrCreateInstance("delay::memory:", db_config, true);
		}
		const auto end_time = steady_clock::now();
		second_creation_was_quick = duration_cast<seconds>(end_time - start_time).count() < 5;
	}};
	std::this_thread::sleep_for(seconds(2));

	auto opening_slow_db_takes_remaining_time = false;
	std::thread t2 {[&instance_cache, &opening_slow_db_takes_remaining_time]() {
		DBConfig db_config;
		const auto start_time = steady_clock::now();
		instance_cache.GetOrCreateInstance("delay::memory:", db_config, true);
		const auto end_time = steady_clock::now();
		const auto duration = duration_cast<milliseconds>(end_time - start_time);
		opening_slow_db_takes_remaining_time = duration > seconds(1) && duration < seconds(10);
	}};

	auto no_delay_for_db_creation = true;
	std::thread t3 {[&instance_cache, &no_delay_for_db_creation]() {
		const auto start_time = steady_clock::now();
		DBConfig db_config;
		while (start_time + seconds(3) < steady_clock::now()) {
			auto db_start_time = steady_clock::now();
			instance_cache.GetOrCreateInstance(":memory:", db_config, false);
			no_delay_for_db_creation &= duration_cast<milliseconds>(steady_clock::now() - db_start_time).count() < 1000;
		}
	}};

	t1.join();
	t2.join();
	t3.join();
	if (!second_creation_was_quick) {
		REQUIRE("second_creation_was_quick" == nullptr);
	}
	if (!opening_slow_db_takes_remaining_time) {
		REQUIRE("opening_slow_db_takes_remaining_time" == nullptr);
	}
	if (!no_delay_for_db_creation) {
		REQUIRE("no_delay_for_db_creation" == nullptr);
	}
}

TEST_CASE("Test attaching the same database path from different databases", "[api][.]") {
	DBInstanceCache instance_cache;
	auto test_path = TestCreatePath("instance_cache_reuse.db");

	DBConfig config;
	auto db1 = instance_cache.GetOrCreateInstance(":memory:", config, false);
	auto db2 = instance_cache.GetOrCreateInstance(":memory:", config, false);

	Connection con1(*db1);
	Connection con2(*db2);
	SECTION("Regular ATTACH conflict") {
		string attach_query = "ATTACH '" + test_path + "' AS db_ref";

		REQUIRE_NO_FAIL(con1.Query(attach_query));

		// fails - already attached in db1
		REQUIRE_FAIL(con2.Query(attach_query));

		// if we detach from con1, we can now attach in con2
		REQUIRE_NO_FAIL(con1.Query("DETACH db_ref"));

		REQUIRE_NO_FAIL(con2.Query(attach_query));

		// .. but not in con1 anymore!
		REQUIRE_FAIL(con1.Query(attach_query));
	}
	SECTION("ATTACH IF NOT EXISTS") {
		string attach_query = "ATTACH IF NOT EXISTS '" + test_path + "' AS db_ref";

		REQUIRE_NO_FAIL(con1.Query(attach_query));

		// fails - already attached in db1
		REQUIRE_FAIL(con2.Query(attach_query));
	}
}

TEST_CASE("Test attaching the same database path from different databases in read-only mode", "[api][.]") {
	DBInstanceCache instance_cache;
	auto test_path = TestCreatePath("instance_cache_reuse_readonly.db");

	// create an empty database
	{
		DuckDB db(test_path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE IF NOT EXISTS integers AS FROM (VALUES (1), (2), (3)) t(i)"));
	}

	DBConfig config;
	auto db1 = instance_cache.GetOrCreateInstance(":memory:", config, false);
	auto db2 = instance_cache.GetOrCreateInstance(":memory:", config, false);
	auto db3 = instance_cache.GetOrCreateInstance(":memory:", config, false);

	Connection con1(*db1);
	Connection con2(*db2);
	Connection con3(*db3);

	SECTION("Regular ATTACH conflict") {
		string attach_query = "ATTACH '" + test_path + "' AS db_ref";
		string read_only_attach = attach_query + " (READ_ONLY)";

		REQUIRE_NO_FAIL(con1.Query(read_only_attach));

		// succeeds - we can attach the same database multiple times in read-only mode
		REQUIRE_NO_FAIL(con2.Query(read_only_attach));

		// fails - we cannot attach in read-write
		REQUIRE_FAIL(con3.Query(attach_query));

		// if we detach from con1, we still cannot attach in read-write in con3
		REQUIRE_NO_FAIL(con1.Query("DETACH db_ref"));
		REQUIRE_FAIL(con3.Query(attach_query));

		// but if we detach in con2, we can attach in read-write mode now
		REQUIRE_NO_FAIL(con2.Query("DETACH db_ref"));
		REQUIRE_NO_FAIL(con3.Query(attach_query));

		// and now we can no longer attach in read-only mode
		REQUIRE_FAIL(con1.Query(read_only_attach));
	}
	SECTION("ATTACH IF EXISTS") {
		string attach_query = "ATTACH IF NOT EXISTS '" + test_path + "' AS db_ref";
		string read_only_attach = attach_query + " (READ_ONLY)";

		REQUIRE_NO_FAIL(con1.Query(read_only_attach));

		// succeeds - we can attach the same database multiple times in read-only mode
		REQUIRE_NO_FAIL(con2.Query(read_only_attach));

		// fails - we cannot attach in read-write
		REQUIRE_FAIL(con3.Query(attach_query));

		// if we detach from con1, we still cannot attach in read-write in con3
		REQUIRE_NO_FAIL(con1.Query("DETACH db_ref"));
		REQUIRE_FAIL(con3.Query(attach_query));

		// but if we detach in con2, we can attach in read-write mode now
		REQUIRE_NO_FAIL(con2.Query("DETACH db_ref"));
		REQUIRE_NO_FAIL(con3.Query(attach_query));

		// and now we can no longer attach in read-only mode
		REQUIRE_FAIL(con1.Query(read_only_attach));
	}
}

TEST_CASE("Test instance cache canonicalization", "[api][.]") {
	LocalFileSystem fs;

	DBInstanceCache instance_cache;
	vector<string> equivalent_paths;
	auto test_path = TestCreatePath("instance_cache_canonicalization.db");
	// base path
	equivalent_paths.push_back(test_path);
	// abs path
	equivalent_paths.push_back(fs.JoinPath(FileSystem::GetWorkingDirectory(), test_path));
	// dot dot
	auto dot_dot = TestCreatePath(fs.JoinPath("subdir", "..", "instance_cache_canonicalization.db"));
	equivalent_paths.push_back(dot_dot);
	// dot dot dot
	auto subdirs = fs.JoinPath("subdir", "subdir2", "subdir3", "..", "..", "..", "instance_cache_canonicalization.db");
	auto dot_dot_dot = TestCreatePath(subdirs);
	equivalent_paths.push_back(dot_dot_dot);
	// dots
	auto dots = TestCreatePath(fs.JoinPath(fs.JoinPath(".", "."), "instance_cache_canonicalization.db"));
	equivalent_paths.push_back(dots);
	// dots that point to a real directory
	auto test_dir_path = TestDirectoryPath();
	auto sep = fs.PathSeparator(test_dir_path);
	idx_t dir_count = StringUtil::Split(test_dir_path, sep).size();
	string many_dots_test_path = test_dir_path;
	for (idx_t i = 0; i < dir_count; i++) {
		many_dots_test_path = fs.JoinPath(many_dots_test_path, "..");
	}
	equivalent_paths.push_back(fs.JoinPath(many_dots_test_path, test_dir_path, "instance_cache_canonicalization.db"));

	vector<shared_ptr<DuckDB>> databases;
	for (auto &path : equivalent_paths) {
		DBConfig config;
		auto db = instance_cache.GetOrCreateInstance(path, config, true);
		databases.push_back(std::move(db));
	}
	{
		Connection con(*databases[0]);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE tbl AS SELECT 42 i"));
	}
	// verify that all these databases point to the same path
	for (auto &db : databases) {
		Connection con(*db);
		auto result = con.Query("SELECT * FROM tbl");
		REQUIRE(CHECK_COLUMN(*result, 0, {42}));
	}
}

TEST_CASE("Test database file path manager absolute path", "[api][.]") {
	LocalFileSystem fs;
	DuckDB db;
	Connection con(db);

	auto test_db = TestCreatePath("test_same_db_attach.db");
	auto test_db_abs = fs.JoinPath(FileSystem::GetWorkingDirectory(), test_db);

	// we can attach this once
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + test_db + "' AS db1"));
	// we cannot attach with absolute path, even though our original attach was with relative path
	REQUIRE_FAIL(con.Query("ATTACH '" + test_db_abs + "' AS db2"));
	// after detaching we can attach with absolute path
	REQUIRE_NO_FAIL(con.Query("DETACH db1"));
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + test_db_abs + "' AS db2"));
}

TEST_CASE("Test automatic DB instance caching", "[api][.]") {
	DBInstanceCache instance_cache;
	DBConfig config;

	SECTION("Unnamed in-memory connections are not shared") {
		auto db1 = instance_cache.GetOrCreateInstance(":memory:", config);
		auto db2 = instance_cache.GetOrCreateInstance(":memory:", config);

		Connection con(*db1);
		Connection con2(*db2);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT)"));
		REQUIRE_NO_FAIL(con2.Query("CREATE TABLE t(i INT)"));
	}
	SECTION("Named in-memory connections are shared") {
		auto db1 = instance_cache.GetOrCreateInstance(":memory:abc", config);
		auto db2 = instance_cache.GetOrCreateInstance(":memory:abc", config);

		Connection con(*db1);
		Connection con2(*db2);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT)"));
		REQUIRE_NO_FAIL(con2.Query("SELECT * FROM t"));
	}
}

static void TestInstanceCacheLastHandle(idx_t reopen_iterations) {
	DBInstanceCache instance_cache;
	DBConfig config;
	auto path = TestCreatePath("instance_cache_in_use.db");

	SECTION("A connection keeps the database open") {
		idx_t create_count = 0;
		auto on_create = [&](DuckDB &) {
			create_count++;
		};
		auto db = instance_cache.GetOrCreateInstance(path, config, true, on_create);
		auto con = make_uniq<Connection>(*db);
		REQUIRE_NO_FAIL(con->Query("CREATE TABLE t AS SELECT 42 AS i"));
		weak_ptr<DatabaseInstance> instance = db->instance;
		db.reset();
		REQUIRE(instance_cache.GetInstance(path, config)->instance == instance.lock());

		auto reopened = instance_cache.GetOrCreateInstance(path, config, true, on_create);
		REQUIRE(reopened->instance == instance.lock());
		REQUIRE(create_count == 1);
		auto result = Connection(*reopened).Query("SELECT i FROM t");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
		result.reset();

		// a different configuration is still refused, and the same one is still served
		DBConfig read_only_config;
		read_only_config.options.access_mode = AccessMode::READ_ONLY;
		REQUIRE_THROWS(instance_cache.GetOrCreateInstance(path, read_only_config, true));
		REQUIRE(instance_cache.GetOrCreateInstance(path, config, true)->instance == instance.lock());

		// the instance closes once nothing holds it anymore
		reopened.reset();
		REQUIRE_NO_FAIL(con->Query("SELECT i FROM t"));
		con.reset();
		REQUIRE(instance.expired());
	}
	SECTION("An unfinished result keeps the database open") {
		auto db = instance_cache.GetOrCreateInstance(path, config, true);
		auto con = make_uniq<Connection>(*db);
		auto result = con->Submit("SELECT * FROM range(100000)");
		REQUIRE_NO_FAIL(*result);
		weak_ptr<DatabaseInstance> instance = db->instance;
		con.reset();
		db.reset();
		REQUIRE(!instance.expired());

		auto reopened = instance_cache.GetOrCreateInstance(path, config, true);
		REQUIRE(reopened->instance == instance.lock());
		auto chunk = result->FetchRaw();
		REQUIRE(chunk);
		REQUIRE(chunk->size() > 0);
		result.reset();
	}
	SECTION("A closed database is opened again") {
		auto db = instance_cache.GetOrCreateInstance(path, config, true);
		REQUIRE_NO_FAIL(Connection(*db).Query("CREATE TABLE t AS SELECT 42 AS i"));
		weak_ptr<DatabaseInstance> instance = db->instance;
		db.reset();
		REQUIRE(instance.expired());
		auto reopened = instance_cache.GetOrCreateInstance(path, config, true);
		auto result = Connection(*reopened).Query("SELECT i FROM t");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
	SECTION("A reopen races with closing the last connection") {
		for (idx_t i = 0; i < reopen_iterations; i++) {
			auto db = instance_cache.GetOrCreateInstance(path, config, true);
			auto con = make_uniq<Connection>(*db);
			db.reset();
			shared_ptr<DuckDB> reopened;
			std::exception_ptr reopen_error;
			std::thread reopener([&]() {
				try {
					DBConfig reopen_config;
					reopened = instance_cache.GetOrCreateInstance(path, reopen_config, true);
				} catch (...) {
					reopen_error = std::current_exception();
				}
			});
			con.reset();
			reopener.join();
			if (reopen_error) {
				std::rethrow_exception(reopen_error);
			}
			REQUIRE_NO_FAIL(Connection(*reopened).Query("SELECT 42"));
		}
	}
	TestDeleteFile(path);
}

TEST_CASE("Test the instance cache hands out a database that is still open after its last handle", "[api]") {
	TestInstanceCacheLastHandle(2);
}

TEST_CASE("Stress the instance cache reopening while its last handle closes", "[api][.]") {
	TestInstanceCacheLastHandle(50);
}

struct SlowShutdownCatalog : public DuckCatalog {
	explicit SlowShutdownCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}
	~SlowShutdownCatalog() override {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
};

struct SlowShutdownStorageExtension : StorageExtension {
	SlowShutdownStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq_base<Catalog, SlowShutdownCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

TEST_CASE("Test reopening a database from several threads while it shuts down", "[api][.]") {
	DBInstanceCache instance_cache;
	const string path = "slow_shutdown::memory:";
	constexpr idx_t REOPENER_COUNT = 4;
	for (idx_t i = 0; i < 20; i++) {
		DBConfig config;
		StorageExtension::Register(config, "slow_shutdown", make_shared_ptr<SlowShutdownStorageExtension>());
		auto db = instance_cache.GetOrCreateInstance(path, config, true);
		weak_ptr<DatabaseInstance> instance = db->instance;

		// the slow catalog gives the reopeners time to look up the entry during shutdown
		vector<shared_ptr<DuckDB>> reopened(REOPENER_COUNT);
		vector<std::exception_ptr> reopen_errors(REOPENER_COUNT);
		vector<std::thread> reopeners;
		for (idx_t reopener_idx = 0; reopener_idx < REOPENER_COUNT; reopener_idx++) {
			reopeners.emplace_back([&, reopener_idx]() {
				try {
					while (!instance.expired()) {
					}
					DBConfig reopen_config;
					StorageExtension::Register(reopen_config, "slow_shutdown",
					                           make_shared_ptr<SlowShutdownStorageExtension>());
					reopened[reopener_idx] = instance_cache.GetOrCreateInstance(path, reopen_config, true);
				} catch (...) {
					reopen_errors[reopener_idx] = std::current_exception();
				}
			});
		}
		db.reset();
		for (auto &reopener : reopeners) {
			reopener.join();
		}
		for (auto &reopen_error : reopen_errors) {
			if (reopen_error) {
				std::rethrow_exception(reopen_error);
			}
		}
		// they all share the one new instance
		for (auto &reopened_db : reopened) {
			REQUIRE(reopened_db->instance == reopened[0]->instance);
		}
	}
}

//! The outcome of a cache call made on another thread, checked on the test thread as Catch assertions are not
//! thread-safe
struct BackgroundCacheCall {
	shared_ptr<DuckDB> db;
	std::exception_ptr error;
	//! The call had not returned within the wait, as a lookup that spins on a kept connection does not
	bool timed_out = false;
};

//! Waits up to 30 seconds for the call behind `returned`, then runs `unblock` when it has not returned, so that a
//! lookup that spins can finish and its thread can be joined
static void WaitForBackgroundCall(std::future<void> &returned, BackgroundCacheCall &result, std::thread &caller,
                                  const std::function<void()> &unblock) {
	if (returned.wait_for(std::chrono::seconds(30)) == std::future_status::timeout) {
		result.timed_out = true;
		unblock();
	}
	caller.join();
}

//! Runs `call` on another thread and waits for it, see WaitForBackgroundCall
template <class CALL>
static BackgroundCacheCall RunInBackground(CALL call, const std::function<void()> &unblock) {
	BackgroundCacheCall result;
	std::promise<void> returned;
	auto returned_future = returned.get_future();
	std::thread caller([&]() {
		try {
			result.db = call();
		} catch (...) {
			result.error = std::current_exception();
		}
		returned.set_value();
	});
	WaitForBackgroundCall(returned_future, result, caller, unblock);
	return result;
}

TEST_CASE("Test the instance cache after an on_create failure that keeps a connection", "[api]") {
	DBInstanceCache instance_cache;
	DBConfig config;
	// the connection keeps the instance the failed callback was given alive, outside of the cache
	unique_ptr<Connection> kept_con;
	auto failing_on_create = [&](DuckDB &created) {
		kept_con = make_uniq<Connection>(created);
		throw InvalidInputException("on_create failed on purpose");
	};
	auto release_kept_connection = [&]() {
		kept_con.reset();
	};

	SECTION("A lookup and a new instance do not wait for the connection") {
		const string path = ":memory:failed_on_create";
		REQUIRE_THROWS_WITH(instance_cache.CreateInstance(path, config, true, failing_on_create),
		                    Catch::Contains("on_create failed on purpose"));
		REQUIRE(kept_con);
		weak_ptr<DatabaseInstance> failed_instance = kept_con->context->db;

		// nothing is cached for the path, and finding that out does not wait for the kept connection
		auto lookup =
		    RunInBackground([&]() { return instance_cache.GetInstance(path, config); }, release_kept_connection);
		REQUIRE(!lookup.timed_out);
		REQUIRE(!lookup.error);
		REQUIRE(!lookup.db);

		// the next call creates a new instance, the failed one lives on outside of the cache
		auto reopen = RunInBackground(
		    [&]() {
			    DBConfig reopen_config;
			    return instance_cache.GetOrCreateInstance(path, reopen_config, true);
		    },
		    release_kept_connection);
		REQUIRE(!reopen.timed_out);
		if (reopen.error) {
			std::rethrow_exception(reopen.error);
		}
		REQUIRE(reopen.db);
		REQUIRE(reopen.db->instance != failed_instance.lock());
		REQUIRE_NO_FAIL(Connection(*reopen.db).Query("CREATE TABLE t AS SELECT 42 AS i"));
		auto found = instance_cache.GetInstance(path, config);
		REQUIRE(found);
		REQUIRE(found->instance == reopen.db->instance);

		// releasing the kept connection closes the failed instance and leaves the cached one alone
		kept_con.reset();
		REQUIRE(failed_instance.expired());
		found = instance_cache.GetInstance(path, config);
		REQUIRE(found);
		REQUIRE(found->instance == reopen.db->instance);
		auto result = Connection(*reopen.db).Query("SELECT i FROM t");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}
	SECTION("CreateInstance works again right after the failure") {
		const string path = ":memory:failed_on_create_again";
		REQUIRE_THROWS_WITH(instance_cache.CreateInstance(path, config, true, failing_on_create),
		                    Catch::Contains("on_create failed on purpose"));
		REQUIRE(kept_con);
		weak_ptr<DatabaseInstance> failed_instance = kept_con->context->db;

		DBConfig again_config;
		auto again = instance_cache.CreateInstance(path, again_config, true);
		REQUIRE(again);
		REQUIRE(again->instance != failed_instance.lock());
		REQUIRE_NO_FAIL(Connection(*again).Query("SELECT 42"));
		auto found = instance_cache.GetInstance(path, config);
		REQUIRE(found);
		REQUIRE(found->instance == again->instance);
	}
	SECTION("A file stays in use until the connection is released") {
		auto path = TestCreatePath("instance_cache_failed_on_create.db");
		REQUIRE_THROWS_WITH(instance_cache.CreateInstance(path, config, true, failing_on_create),
		                    Catch::Contains("on_create failed on purpose"));
		REQUIRE(kept_con);
		weak_ptr<DatabaseInstance> failed_instance = kept_con->context->db;

		// the kept connection holds the file, so opening it again fails at once instead of waiting
		auto reopen = RunInBackground(
		    [&]() {
			    DBConfig reopen_config;
			    return instance_cache.GetOrCreateInstance(path, reopen_config, true);
		    },
		    release_kept_connection);
		REQUIRE(!reopen.timed_out);
		REQUIRE(!reopen.db);
		REQUIRE(reopen.error);
		REQUIRE_THROWS_WITH(std::rethrow_exception(reopen.error), Catch::Contains("Unique file handle conflict"));

		// once it is released, the next call opens the file again
		kept_con.reset();
		REQUIRE(failed_instance.expired());
		DBConfig reopen_config;
		auto reopened = instance_cache.GetOrCreateInstance(path, reopen_config, true);
		REQUIRE(reopened);
		REQUIRE_NO_FAIL(Connection(*reopened).Query("SELECT 42"));
		reopened.reset();
		TestDeleteFile(path);
	}
	SECTION("A call that waits for the callback creates its own instance") {
		const string path = ":memory:failed_on_create_waiter";
		BackgroundCacheCall waiter;
		std::promise<void> waiter_returned;
		auto waiter_returned_future = waiter_returned.get_future();
		std::thread waiter_thread;
		bool waiter_holds_entry = false;
		auto failing_on_create_with_waiter = [&](DuckDB &created) {
			kept_con = make_uniq<Connection>(created);
			// the waiter holds the entry once it has found it, and waits for this callback to end from then on
			auto &entry = DBConfig::GetConfig(*created.instance).db_cache_entry;
			auto holders_before = entry.use_count();
			waiter_thread = std::thread([&]() {
				try {
					DBConfig waiter_config;
					waiter.db = instance_cache.GetOrCreateInstance(path, waiter_config, true);
				} catch (...) {
					waiter.error = std::current_exception();
				}
				waiter_returned.set_value();
			});
			auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (entry.use_count() <= holders_before && std::chrono::steady_clock::now() < deadline) {
				std::this_thread::yield();
			}
			waiter_holds_entry = entry.use_count() > holders_before;
			throw InvalidInputException("on_create failed on purpose");
		};
		// the outcome is checked after the waiter is joined, a failing assertion must not unwind past a joinable thread
		string creator_error;
		try {
			instance_cache.GetOrCreateInstance(path, config, true, failing_on_create_with_waiter);
		} catch (std::exception &ex) {
			creator_error = ex.what();
		}
		if (waiter_thread.joinable()) {
			WaitForBackgroundCall(waiter_returned_future, waiter, waiter_thread, release_kept_connection);
		}
		REQUIRE_THAT(creator_error, Catch::Contains("on_create failed on purpose"));
		REQUIRE(waiter_holds_entry);
		REQUIRE(!waiter.timed_out);
		if (waiter.error) {
			std::rethrow_exception(waiter.error);
		}
		REQUIRE(waiter.db);
		REQUIRE(kept_con);
		weak_ptr<DatabaseInstance> failed_instance = kept_con->context->db;
		REQUIRE(waiter.db->instance != failed_instance.lock());
		REQUIRE_NO_FAIL(Connection(*waiter.db).Query("SELECT 42"));
		auto found = instance_cache.GetInstance(path, config);
		REQUIRE(found);
		REQUIRE(found->instance == waiter.db->instance);
	}
}
