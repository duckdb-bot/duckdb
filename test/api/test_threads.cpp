#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "duckdb/storage/storage_info.hpp"

#include <chrono>
#include <condition_variable>
#include <thread>
#if defined(__GLIBC__)
#include <pthread.h>
#include <sched.h>
#endif

using namespace duckdb;

void run_query_multiple_times(duckdb::unique_ptr<string> query, duckdb::unique_ptr<Connection> con) {
	for (int i = 0; i < 10; ++i) {
		auto result = con->Query(*query);
	}
}

void change_thread_counts(duckdb::DuckDB &db) {
	auto con = Connection(db);
	for (int i = 0; i < 10; ++i) {
		con.Query("SET threads=10");
		con.Query("SET threads=1");
	}
}

// NumberOfThreads acquired the same lock as RelaunchThreads
// NumberOfThreads is waiting for the lock
// RelaunchThreads is waiting on the thread to finish, while holding the lock
TEST_CASE("Test deadlock issue between NumberOfThreads and RelaunchThreads", "[api]") {
	duckdb::DuckDB db(nullptr);

	int thread_count = 10;
	std::vector<std::thread> threads(thread_count);

	// This query will hit NumberOfThreads because it uses the RadixPartitionedHashtable
	for (int i = 0; i < thread_count; ++i) {
		auto query = make_uniq<string>(R"(
			WITH dataset AS (
			  SELECT * FROM (VALUES
				(1, 'Alice'),
				(2, 'Bob'),
				(3, 'Alice'),
				(4, 'Carol')
			  ) AS t(id, name)
			)
			SELECT DISTINCT name FROM dataset;
		)");

		threads[i] = std::thread(run_query_multiple_times, std::move(query), make_uniq<Connection>(db));
	}

	// Fire off queries that change the thread count,
	// causing us to relaunch the worker threads on every subsequent query.
	change_thread_counts(db);

	for (int i = 0; i < thread_count; ++i) {
		threads[i].join();
	}
}

TEST_CASE("Test database maximum_threads argument", "[api]") {
	// default is number of hw threads
	// FIXME: not yet
	{
		DuckDB db(nullptr);
		auto file_system = make_uniq<VirtualFileSystem>();
		REQUIRE(db.NumberOfThreads() == DBConfig().GetSystemMaxThreads(*file_system));
	}
	// but we can set another value
	{
		DBConfig config;
		config.options.maximum_threads = 10;
		DuckDB db(nullptr, &config);
		REQUIRE(db.NumberOfThreads() == 10);
	}
	// zero is not erlaubt
	{
		DBConfig config;
		config.options.maximum_threads = 0;
		DuckDB db;
		REQUIRE_THROWS(db = DuckDB(nullptr, &config));
	}
}

TEST_CASE("Test external threads", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &config = DBConfig::GetConfig(*db.instance);
	auto options = config.GetOptions();

	con.Query("SET threads=13");
	REQUIRE(config.options.maximum_threads == 13);
	REQUIRE(db.NumberOfThreads() == 13);
	con.Query("SET external_threads=13");
	REQUIRE(Settings::Get<ExternalThreadsSetting>(config) == 13);
	REQUIRE(db.NumberOfThreads() == 13);

	con.Query("SET external_threads=0");
	REQUIRE(Settings::Get<ExternalThreadsSetting>(config) == 0);
	REQUIRE(db.NumberOfThreads() == 13);

	auto res = con.Query("SET external_threads=-1");
	REQUIRE(res->HasError());
	REQUIRE(StringUtil::Contains(res->GetError(), "out of range"));

	res = con.Query("SET external_threads=14");
	REQUIRE(res->HasError());
	REQUIRE(StringUtil::Contains(res->GetError(), "smaller"));

	con.Query("SET external_threads=5");
	REQUIRE(Settings::Get<ExternalThreadsSetting>(config) == 5);
	REQUIRE(db.NumberOfThreads() == 13);

	con.Query("RESET external_threads");
	REQUIRE(Settings::Get<ExternalThreadsSetting>(config) == 1);
	REQUIRE(db.NumberOfThreads() == 13);

	con.Query("RESET threads");
	auto file_system = make_uniq<VirtualFileSystem>();
	REQUIRE(config.options.maximum_threads == DBConfig().GetSystemMaxThreads(*file_system));
	REQUIRE(db.NumberOfThreads() == DBConfig().GetSystemMaxThreads(*file_system));
}

#ifndef DUCKDB_NO_THREADS
namespace {

struct ThreadCapSourceState : public GlobalSourceState {
	explicit ThreadCapSourceState(idx_t limit_p) : limit(limit_p) {
	}
	idx_t limit;

	idx_t MaxThreads() override {
		return limit;
	}
};

struct ThreadCapOperatorState : public GlobalOperatorState {
	explicit ThreadCapOperatorState(idx_t limit_p) : limit(limit_p) {
	}
	idx_t limit;

	idx_t MaxThreads(idx_t source_max_threads) override {
		return limit;
	}
};

struct ThreadCapSinkState : public GlobalSinkState {
	explicit ThreadCapSinkState(idx_t limit_p) : limit(limit_p) {
	}
	idx_t limit;
	idx_t calls = 0;
	idx_t input_limit = 0;

	idx_t MaxThreads(idx_t source_max_threads) override {
		calls++;
		input_limit = source_max_threads;
		return limit;
	}
};

class ThreadCapOperator : public PhysicalOperator {
public:
	ThreadCapOperator(PhysicalPlan &plan, idx_t limit_p)
	    : PhysicalOperator(plan, PhysicalOperatorType::INVALID, {LogicalType::INTEGER}, 0), limit(limit_p) {
	}

	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override {
		return make_uniq<ThreadCapSourceState>(limit);
	}
	unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const override {
		return make_uniq<ThreadCapOperatorState>(limit);
	}
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<ThreadCapSinkState>(limit);
	}
	bool IsSource() const override {
		return true;
	}
	bool IsSink() const override {
		return true;
	}
	bool ParallelSource() const override {
		return true;
	}
	bool ParallelOperator() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}

private:
	idx_t limit;
};

} // namespace

// SQL cannot supply a sink callback that raises the accumulated source/operator limit.
TEST_CASE("Pipeline sink thread limits cannot raise source or operator caps", "[api]") {
	DBConfig config;
	config.options.maximum_threads = 8;
	DuckDB db(nullptr, &config);
	Connection con(db);
	Executor executor(*con.context);
	PipelineBuildState build_state;
	struct Limits {
		idx_t source, op, sink, expected, sink_input;
	};
	const Limits cases[] = {{1, 8, 8, 1, 1}, {8, 2, 8, 2, 2}, {8, 8, 3, 3, 8},
	                        {0, 8, 8, 0, 0}, {8, 8, 0, 0, 8}, {16, 16, 16, 8, 8}};
	for (const auto &limits : cases) {
		CAPTURE(limits.source, limits.op, limits.sink);
		PhysicalPlan plan(Allocator::Get(*con.context));
		auto &source = plan.Make<ThreadCapOperator>(limits.source);
		auto &op = plan.Make<ThreadCapOperator>(limits.op);
		auto &sink = plan.Make<ThreadCapOperator>(limits.sink);
		Pipeline pipeline(executor);
		build_state.SetPipelineSource(pipeline, source);
		build_state.SetPipelineOperators(pipeline, {op});
		build_state.SetPipelineSink(pipeline, sink, 1);
		pipeline.Ready();
		pipeline.Reset();
		REQUIRE(pipeline.GetMaxThreads() == limits.expected);
		auto &sink_state = sink.sink_state->Cast<ThreadCapSinkState>();
		REQUIRE(sink_state.calls == 1);
		REQUIRE(sink_state.input_limit == limits.sink_input);
	}
}

TEST_CASE("Test async threads", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &scheduler = TaskScheduler::GetScheduler(*con.context);

	con.Query("SET async_threads=0");
	REQUIRE(scheduler.NumberOfAsyncThreads() == 0);

	con.Query("SET async_threads=2");
	REQUIRE(scheduler.NumberOfAsyncThreads() == 2);
}

#ifdef __linux__
static unordered_set<string> ProcessThreadIds() {
	LocalFileSystem fs;
	unordered_set<string> ids;
	REQUIRE(fs.ListFiles("/proc/self/task", [&](const string &id, bool) { ids.insert(id); }));
	return ids;
}

// threads of the process that did not exist yet, so that threads exiting in the meantime do not count
static idx_t NewThreadCount(const unordered_set<string> &threads_before) {
	idx_t count = 0;
	for (auto &id : ProcessThreadIds()) {
		if (threads_before.find(id) == threads_before.end()) {
			count++;
		}
	}
	return count;
}

TEST_CASE("Test async threads are launched on their first use", "[api]") {
	const auto threads_before = ProcessThreadIds();
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 16;
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range AS i FROM range(10000)"));
	auto result = con.Query("SELECT sum(i) FROM t");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::HUGEINT(49995000)}));
	REQUIRE(NewThreadCount(threads_before) < 16);

	auto &scheduler = TaskScheduler::GetScheduler(*con.context);
	REQUIRE(scheduler.NumberOfAsyncThreads() == 16);
	REQUIRE(NewThreadCount(threads_before) >= 16);
}

TEST_CASE("Test idle memory reclamation does not launch the async threads", "[api]") {
	const auto threads_before = ProcessThreadIds();
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 16;
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range AS i FROM range(10000)"));
	// what a worker runs after it was idle for the decay delay
	auto &allocator = BlockAllocator::Get(*db.instance);
	allocator.ThreadIdle();
	REQUIRE(NewThreadCount(threads_before) < 16);
	auto result = con.Query("SELECT sum(i) FROM t");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::HUGEINT(49995000)}));

	REQUIRE(TaskScheduler::GetScheduler(*con.context).NumberOfAsyncThreads() == 16);
	allocator.ThreadIdle();
	result = con.Query("SELECT sum(i) FROM t");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::HUGEINT(49995000)}));
}

namespace {

class BlockingTask : public BaseExecutorTask {
public:
	BlockingTask(TaskExecutor &executor, mutex &lock_p, std::condition_variable &cv_p, bool &blocked_p,
	             bool &released_p)
	    : BaseExecutorTask(executor), lock(lock_p), cv(cv_p), blocked(blocked_p), released(released_p) {
	}
	void ExecuteTask() override {
		unique_lock<mutex> guard(lock);
		blocked = true;
		cv.notify_all();
		cv.wait(guard, [&]() { return released; });
	}

private:
	mutex &lock;
	std::condition_variable &cv;
	bool &blocked;
	bool &released;
};

} // namespace

TEST_CASE("Test idle workers reclaim memory without launching the async threads", "[api]") {
	constexpr idx_t SIZE = 8 * 1024 * 1024;
	const auto threads_before = ProcessThreadIds();
	DBConfig config;
	config.options.maximum_threads = 2;
	config.options.async_threads = 16;
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto &allocator = BlockAllocator::Get(*db.instance);
	auto pointer = allocator.AllocateData(SIZE);
	pointer[0] = 42;

	// the worker runs this task while the memory is freed, so it cannot reclaim it before the check
	mutex lock;
	std::condition_variable cv;
	bool blocked = false;
	bool released = false;
	TaskExecutor executor(*con.context);
	executor.ScheduleTask(make_uniq<BlockingTask>(executor, lock, cv, blocked, released));
	bool worker_blocked;
	{
		unique_lock<mutex> guard(lock);
		worker_blocked = cv.wait_for(guard, std::chrono::seconds(10), [&]() { return blocked; });
	}
	allocator.FreeData(pointer, SIZE);
	const auto cached = allocator.GetCachedMemory();
	{
		lock_guard<mutex> guard(lock);
		released = true;
	}
	cv.notify_all();
	executor.WorkOnTasks();
	REQUIRE(worker_blocked);
	REQUIRE(cached >= SIZE);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (allocator.GetCachedMemory() != 0 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(allocator.GetCachedMemory() == 0);
	REQUIRE(NewThreadCount(threads_before) < 16);
}

namespace {

class AsyncCountingTask : public BaseExecutorTask {
public:
	AsyncCountingTask(TaskExecutor &executor, atomic<idx_t> &count_p) : BaseExecutorTask(executor), count(count_p) {
	}
	void ExecuteTask() override {
		count++;
	}

private:
	atomic<idx_t> &count;
};

} // namespace

TEST_CASE("Test async threads are launched by the first async task", "[api]") {
	// one task goes through ScheduleTask, more through ScheduleTasks
	for (idx_t task_count : {1, 8}) {
		const auto threads_before = ProcessThreadIds();
		DBConfig config;
		config.options.maximum_threads = 1;
		config.options.async_threads = 16;
		DuckDB db(nullptr, &config);
		Connection con(db);
		REQUIRE(NewThreadCount(threads_before) < 16);

		atomic<idx_t> count {0};
		TaskExecutor executor(*con.context, TaskSchedulerType::ASYNC);
		vector<unique_ptr<BaseExecutorTask>> tasks;
		for (idx_t i = 0; i < task_count; i++) {
			tasks.push_back(make_uniq<AsyncCountingTask>(executor, count));
		}
		executor.ScheduleTasks(std::move(tasks));
		REQUIRE(NewThreadCount(threads_before) >= 16);
		executor.WorkOnTasks();
		REQUIRE(count == task_count);
	}
}
#if defined(__GLIBC__)
TEST_CASE("Test async threads launched from a pinned thread run on every CPU", "[api]") {
	cpu_set_t process_cpus;
	REQUIRE(sched_getaffinity(0, sizeof(process_cpus), &process_cpus) == 0);
	if (CPU_COUNT(&process_cpus) < 2) {
		return;
	}
	DBConfig config;
	config.options.maximum_threads = 1;
	config.options.async_threads = 4;
	DuckDB db(nullptr, &config);
	Connection con(db);
	const auto threads_before = ProcessThreadIds();

	// launch the async threads from a thread that is pinned to one CPU, like a pinned worker
	int first_cpu = 0;
	while (!CPU_ISSET(first_cpu, &process_cpus)) {
		first_cpu++;
	}
	cpu_set_t one_cpu;
	CPU_ZERO(&one_cpu);
	CPU_SET(first_cpu, &one_cpu);
	REQUIRE(pthread_setaffinity_np(pthread_self(), sizeof(one_cpu), &one_cpu) == 0);
	auto async_threads = TaskScheduler::GetScheduler(*con.context).NumberOfAsyncThreads();
	REQUIRE(pthread_setaffinity_np(pthread_self(), sizeof(process_cpus), &process_cpus) == 0);
	REQUIRE(async_threads == 4);

	// each async thread sets its CPUs when it starts
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	idx_t threads_on_every_cpu = 0;
	while (threads_on_every_cpu < 4 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		threads_on_every_cpu = 0;
		for (auto &id : ProcessThreadIds()) {
			if (threads_before.find(id) != threads_before.end()) {
				continue;
			}
			cpu_set_t thread_cpus;
			if (sched_getaffinity(std::stoi(id), sizeof(thread_cpus), &thread_cpus) == 0 &&
			    CPU_EQUAL(&thread_cpus, &process_cpus)) {
				threads_on_every_cpu++;
			}
		}
	}
	REQUIRE(threads_on_every_cpu >= 4);
}
#endif
#endif

TEST_CASE("Test launching async threads while the thread count changes", "[api]") {
	auto path = TestCreatePath("async_threads_launch.parquet");
	{
		DuckDB db(nullptr);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("COPY (SELECT range AS i FROM range(100000)) TO '" + path + "'"));
	}
	for (idx_t i = 0; i < 5; i++) {
		DBConfig config;
		config.options.async_threads = 4;
		DuckDB db(nullptr, &config);
		std::vector<std::thread> threads;
		vector<string> results(4);
		for (idx_t t = 0; t < 4; t++) {
			threads.emplace_back([&db, &path, &results, t]() {
				Connection con(db);
				for (idx_t q = 0; q < 5; q++) {
					auto result = con.Query("SELECT sum(i)::VARCHAR FROM '" + path + "'");
					if (result->HasError()) {
						results[t] += result->GetError();
						continue;
					}
					for (auto &row : *result) {
						results[t] += row.GetValue<string>(0) + ";";
					}
				}
			});
		}
		change_thread_counts(db);
		for (auto &thread : threads) {
			thread.join();
		}
		for (auto &result : results) {
			REQUIRE(result == StringUtil::Repeat("4999950000;", 5));
		}
	}
}

static idx_t ColumnDataScanMaxThreads(Connection &con, QueryResult &result,
                                      const OperatorPartitionInfo &partition_info) {
	PhysicalPlan physical_plan(Allocator::Get(*con.context));
	auto &scan = physical_plan.Make<PhysicalColumnDataScan>(result.GetTypes(), PhysicalOperatorType::COLUMN_DATA_SCAN,
	                                                        result.Collection().Count(), result.Collection());
	auto source_state = scan.GetGlobalSourceState(*con.context, partition_info);
	return source_state->MaxThreads();
}

#if STANDARD_VECTOR_SIZE <= 4096
TEST_CASE("Column data scan batch sizes preserve source parallelism", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=8"));

	const idx_t small_count = STANDARD_VECTOR_SIZE * 16;
	auto small_result = con.Query(StringUtil::Format("SELECT i FROM range(%llu) t(i)", small_count));
	REQUIRE(!small_result->HasError());
	auto &small_materialized = *small_result;

	REQUIRE(ColumnDataScanMaxThreads(con, small_materialized, OperatorPartitionInfo::NoPartitionInfo()) == 16);
	REQUIRE(ColumnDataScanMaxThreads(con, small_materialized, OperatorPartitionInfo::BatchIndex()) == 8);
	REQUIRE(ColumnDataScanMaxThreads(con, small_materialized,
	                                 OperatorPartitionInfo::BatchIndex(DEFAULT_ROW_GROUP_SIZE)) == 8);

#if STANDARD_VECTOR_SIZE >= 512
	const idx_t large_count = DEFAULT_ROW_GROUP_SIZE * 8 + STANDARD_VECTOR_SIZE;
	auto large_result = con.Query(StringUtil::Format("SELECT i FROM range(%llu) t(i)", large_count));
	REQUIRE(!large_result->HasError());
	auto &large_materialized = *large_result;
	REQUIRE(ColumnDataScanMaxThreads(con, large_materialized, OperatorPartitionInfo::BatchIndex()) == 9);
	REQUIRE(ColumnDataScanMaxThreads(con, large_materialized,
	                                 OperatorPartitionInfo::BatchIndex(DEFAULT_ROW_GROUP_SIZE)) == 9);
#endif
}
#endif
#endif

#ifdef DUCKDB_NO_THREADS
TEST_CASE("Test scheduling with no threads", "[api]") {
	DuckDB db(nullptr);
	Connection con1(db);
	Connection con2(db);

	const auto query_1 = con1.Submit("SELECT 42");
	const auto query_2 = con2.Submit("SELECT 42");
	// Get the completed pipelines. Because "executeTask" was never called, there should be no completed pipelines.
	auto query_1_pipelines = con1.context->GetExecutor().GetCompletedPipelines();
	REQUIRE((query_1_pipelines == 0));

	// Execute the second query
	query_2->Complete();
	REQUIRE_NO_FAIL(*query_2);

	// And even after that, there should still be no completed pipelines for the first query.
	query_1_pipelines = con1.context->GetExecutor().GetCompletedPipelines();
	REQUIRE((query_1_pipelines == 0));
	query_1->Complete();
	REQUIRE_NO_FAIL(*query_1);
}
#endif
