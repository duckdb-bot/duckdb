#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"

#include <thread>

using namespace duckdb;

// Many short queries that each build and drop small hash tables, so most nodes of the eviction queues die
DUCKDB_BENCHMARK(ShortJoinQueries, "[micro]")
static constexpr idx_t JOIN_COUNT = 6;
static constexpr idx_t QUERY_COUNT = 2000;
string query;
void Load(DuckDBBenchmarkState *state) override {
	state->conn.Query("SET threads = 2");
	state->conn.Query(
	    "CREATE TABLE src AS SELECT i AS id, 'k' || (i % 500) AS jkey, 'v' || i AS c1 FROM range(1000) t(i)");
	query = "SELECT src.id, src.c1 FROM src";
	for (idx_t j = 0; j < JOIN_COUNT; j++) {
		auto table = "t" + to_string(j);
		state->conn.Query("CREATE TABLE " + table +
		                  " AS SELECT 'k' || (i % 500) AS jkey, 'x_' || i AS val FROM range(1000) t(i)");
		query += " LEFT JOIN " + table + " ON src.jkey = " + table + ".jkey";
	}
	query += " LIMIT 100";
}
void RunBenchmark(DuckDBBenchmarkState *state) override {
	for (idx_t i = 0; i < QUERY_COUNT; i++) {
		state->result = state->conn.Query(query);
	}
}
string VerifyResult(QueryResult *result) override {
	if (result->HasError()) {
		return result->GetError();
	}
	auto row_count = result->RowCount();
	if (row_count != 100) {
		return "expected 100 rows, got " + to_string(row_count);
	}
	return string();
}
string BenchmarkInfo() override {
	return "Run a query with six LEFT JOINs of small tables 2000 times";
}
FINISH_BENCHMARK(ShortJoinQueries)

// Threads allocate and release small destroyable buffers, next to a few that stay alive in the eviction queues
DUCKDB_BENCHMARK(ManagedBufferChurn, "[micro]")
static constexpr idx_t THREAD_COUNT = 4;
static constexpr idx_t BUFFERS_PER_THREAD = 50000;
static constexpr idx_t LIVE_BUFFER_INTERVAL = 1000;
void Load(DuckDBBenchmarkState *state) override {
}
void RunBenchmark(DuckDBBenchmarkState *state) override {
	auto &buffer_manager = BufferManager::GetBufferManager(*state->conn.context);
	vector<std::thread> threads;
	for (idx_t t = 0; t < THREAD_COUNT; t++) {
		threads.emplace_back([&buffer_manager]() {
			vector<shared_ptr<BlockHandle>> live_blocks;
			for (idx_t i = 0; i < BUFFERS_PER_THREAD; i++) {
				auto pin = buffer_manager.Allocate(MemoryTag::EXTENSION, 64 * 1024, true);
				if (i % LIVE_BUFFER_INTERVAL == 0) {
					live_blocks.push_back(pin.GetBlockHandle());
				}
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
}
string VerifyResult(QueryResult *result) override {
	return string();
}
string BenchmarkInfo() override {
	return "Four threads each allocate and release 50000 buffers of 64 KiB, keeping every 1000th one";
}
FINISH_BENCHMARK(ManagedBufferChurn)

// One thread pins and unpins the same few blocks over and over, so the eviction queue fills with stale nodes
struct ManagedBufferRepinState : public DuckDBBenchmarkState {
	ManagedBufferRepinState() : DuckDBBenchmarkState(string()) {
	}
	//! Destroyed before the database
	vector<shared_ptr<BlockHandle>> blocks;
};

DUCKDB_BENCHMARK(ManagedBufferRepin, "[micro]")
static constexpr idx_t BLOCK_COUNT = 64;
static constexpr idx_t ROUND_COUNT = 2000;
idx_t invalid_pins = 0;
unique_ptr<DuckDBBenchmarkState> CreateBenchmarkState() override {
	return make_uniq<ManagedBufferRepinState>();
}
void Load(DuckDBBenchmarkState *state_p) override {
	auto &state = static_cast<ManagedBufferRepinState &>(*state_p);
	auto &buffer_manager = BufferManager::GetBufferManager(*state.conn.context);
	for (idx_t i = 0; i < BLOCK_COUNT; i++) {
		auto pin = buffer_manager.Allocate(MemoryTag::EXTENSION, 64 * 1024, true);
		state.blocks.push_back(pin.GetBlockHandle());
	}
}
void RunBenchmark(DuckDBBenchmarkState *state_p) override {
	auto &state = static_cast<ManagedBufferRepinState &>(*state_p);
	auto &buffer_manager = BufferManager::GetBufferManager(*state.conn.context);
	invalid_pins = 0;
	for (idx_t round = 0; round < ROUND_COUNT; round++) {
		for (auto &block : state.blocks) {
			auto pin = buffer_manager.Pin(block);
			if (!pin.IsValid()) {
				invalid_pins++;
			}
		}
	}
}
string VerifyResult(QueryResult *result) override {
	if (invalid_pins > 0) {
		return to_string(invalid_pins) + " pins of evicted blocks";
	}
	return string();
}
string BenchmarkInfo() override {
	return "Pin and unpin 64 buffers of 64 KiB 2000 times each";
}
FINISH_BENCHMARK(ManagedBufferRepin)
