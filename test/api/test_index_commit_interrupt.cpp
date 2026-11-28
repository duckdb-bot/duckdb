#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/storage_info.hpp"

using namespace duckdb;

namespace {

//! What the key of the first index of a table does to a client, and how often that key was evaluated. The key of
//! the second index sorts lists, and is evaluated for the same chunk after the hook has acted
struct KeyHook : public ScalarFunctionInfo {
	//! The client that the hook interrupts, or whose query it lets run out of time, while the hook is armed
	optional_ptr<ClientContext> client;
	//! The call that triggers the hook. Without one, every chunk that holds an id divisible by four triggers it
	idx_t trigger_call = 0;
	//! Whether the hook sets a deadline in the past instead of the interrupt
	bool expire = false;
	//! The number of calls of the key, and the number of calls that triggered the hook
	atomic<idx_t> calls {0};
	atomic<idx_t> triggered {0};

	void Arm(Connection &con, idx_t trigger_call_p, bool expire_p = false) {
		calls = 0;
		triggered = 0;
		trigger_call = trigger_call_p;
		expire = expire_p;
		client = con.context.get();
	}

	void Disarm() {
		if (client && expire) {
			client->query_deadline.SetInvalid();
		}
		client = nullptr;
	}
};

void HookedKey(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &hook = state.expr.Cast<BoundFunctionExpression>().Function().GetExtraFunctionInfo().Cast<KeyHook>();
	auto call = ++hook.calls;
	if (hook.client) {
		bool trigger = call == hook.trigger_call;
		if (hook.trigger_call == 0) {
			for (auto entry : args.data[0].Values<int32_t>()) {
				if (entry.IsValid() && entry.GetValue() % 4 == 0) {
					trigger = true;
					break;
				}
			}
		}
		if (trigger) {
			hook.triggered++;
			if (hook.expire) {
				hook.client->query_deadline = 1;
			} else {
				hook.client->Interrupt();
			}
		}
	}
	result.Reference(args.data[0]);
}

shared_ptr<KeyHook> CreateHookedKey(Connection &con) {
	auto hook = make_shared_ptr<KeyHook>();
	ScalarFunction function("hooked_key", {LogicalType::INTEGER}, LogicalType::INTEGER, HookedKey);
	function.SetExtraFunctionInfo(hook);
	CreateScalarFunctionInfo info(function);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
	return hook;
}

//! The rows of the tables before a test changes them, with the ids and the keys 0 to 999
constexpr idx_t INITIAL_ROWS = 1000;
//! The rows that a transaction of the tests adds. Five chunks, so that a hook at the third call of the key acts
//! while chunks are still to come
constexpr idx_t MORE_ROWS = 5 * STANDARD_VECTOR_SIZE;
//! The chunks of the commit that runs out of time. A sort looks at the clock once in 256 checks and checks at
//! least once per chunk, and every chunk is appended and taken back
constexpr idx_t DEADLINE_CHUNKS = 150;

//! A table with a first index that calls the hook and a second, unique index on a sorted list
void CreateIndexedTable(Connection &con, const string &name, idx_t rows = INITIAL_ROWS) {
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE " + name + " (id INTEGER, l INTEGER[])"));
	REQUIRE_NO_FAIL(con.Query("CREATE INDEX " + name + "_first ON " + name + " (hooked_key(id))"));
	REQUIRE_NO_FAIL(con.Query("CREATE UNIQUE INDEX " + name + "_second ON " + name + " ((list_sort(l)[1]))"));
	REQUIRE_NO_FAIL(
	    con.Query("INSERT INTO " + name + " SELECT i, [i + 1, i] FROM range(" + to_string(rows) + ") r(i)"));
}

//! Adds rows with the ids and the keys that follow those of the initial rows
void InsertMoreRows(Connection &con, const string &name, idx_t rows = MORE_ROWS) {
	REQUIRE_NO_FAIL(con.Query("INSERT INTO " + name + " SELECT i, [i + 1, i] FROM range(" + to_string(INITIAL_ROWS) +
	                          ", " + to_string(INITIAL_ROWS + rows) + ") r(i)"));
}

//! The INSERT of one row with this key
string InsertKey(const string &name, idx_t key) {
	return "INSERT INTO " + name + " VALUES (-1, [" + to_string(key + 1) + ", " + to_string(key) + "])";
}

void RequireRows(Connection &con, const string &name, idx_t count) {
	auto result = con.Query("SELECT count(*), max(id) FROM " + name);
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(NumericCast<int64_t>(count))}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::INTEGER(NumericCast<int32_t>(count - 1))}));
}

//! The number of calls of the key when a commit of these rows succeeds, counted on a table of its own
idx_t CountCommitCalls(Connection &con, KeyHook &hook, const string &name, idx_t rows) {
	CreateIndexedTable(con, name);
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	InsertMoreRows(con, name, rows);
	hook.calls = 0;
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	return hook.calls;
}

} // namespace

TEST_CASE("An interrupt while a commit maintains an index on sorted lists", "[api][index]") {
	// one thread, so that the calls of the key are counted in the order of the chunks
	auto config = GetTestConfig();
	config->SetOptionByName("threads", Value::BIGINT(1));
	DuckDB db(nullptr, config.get());
	Connection con(db);
	auto hook_ptr = CreateHookedKey(con);
	auto &hook = *hook_ptr;

	CreateIndexedTable(con, "t");

	SECTION("while the commit appends to the indexes") {
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		InsertMoreRows(con, "t");
		// the third chunk raises the interrupt, the lists of that chunk and of the following ones are still sorted
		hook.Arm(con, 3);
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
		hook.Disarm();
		REQUIRE(hook.triggered.load() == 1);
		REQUIRE(hook.calls.load() > 3);
		RequireRows(con, "t", INITIAL_ROWS + MORE_ROWS);
		// every key is in the unique index
		REQUIRE_FAIL(con.Query(InsertKey("t", INITIAL_ROWS / 2)));
		REQUIRE_FAIL(con.Query(InsertKey("t", INITIAL_ROWS + MORE_ROWS - 1)));
		REQUIRE_NO_FAIL(con.Query(InsertKey("t", INITIAL_ROWS + MORE_ROWS)));
	}
	SECTION("while a failed commit is taken back") {
		auto commit_calls = CountCommitCalls(con, hook, "counted", MORE_ROWS);
		REQUIRE(commit_calls > 0);

		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		InsertMoreRows(con, "t");
		REQUIRE_NO_FAIL(con.Query("SET debug_force_commit_failure = true"));
		// the commit fails after its last key, so the first key that is taken back raises the interrupt
		hook.Arm(con, commit_calls + 1);
		auto result = con.Query("COMMIT");
		hook.Disarm();
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "Forced commit failure"));
		REQUIRE(hook.triggered.load() == 1);
		REQUIRE(hook.calls.load() > commit_calls + 1);
		REQUIRE_NO_FAIL(con.Query("SET debug_force_commit_failure = false"));
		RequireRows(con, "t", INITIAL_ROWS);
		// the keys of the rows that were taken back are free again, the keys of the rows that are left are not
		InsertMoreRows(con, "t");
		REQUIRE_FAIL(con.Query(InsertKey("t", INITIAL_ROWS / 2)));
		RequireRows(con, "t", INITIAL_ROWS + MORE_ROWS);
	}
	SECTION("a deadline that passes while a failed commit is taken back") {
		const idx_t rows = DEADLINE_CHUNKS * STANDARD_VECTOR_SIZE;
		auto commit_calls = CountCommitCalls(con, hook, "counted", rows);
		REQUIRE(commit_calls >= DEADLINE_CHUNKS);

		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		InsertMoreRows(con, "t", rows);
		REQUIRE_NO_FAIL(con.Query("SET debug_force_commit_failure = true"));
		// the first key of the commit puts the deadline of the query in the past
		hook.Arm(con, 1, true);
		auto result = con.Query("COMMIT");
		hook.Disarm();
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "Forced commit failure"));
		REQUIRE(hook.triggered.load() == 1);
		// every key was appended and taken back
		REQUIRE(hook.calls.load() >= 2 * commit_calls);
		REQUIRE_NO_FAIL(con.Query("SET debug_force_commit_failure = false"));
		RequireRows(con, "t", INITIAL_ROWS);
		InsertMoreRows(con, "t", rows);
		REQUIRE_FAIL(con.Query(InsertKey("t", INITIAL_ROWS / 2)));
		RequireRows(con, "t", INITIAL_ROWS + rows);
	}
	SECTION("while the commit appends a key that finalizes an ordered aggregate state") {
		// finalize sorts the values of the state with the client that the aggregate was bound with
		const string states = "SELECT i::INTEGER AS id, list(v ORDER BY v) EXPORT_STATE AS s FROM range(%s, %s) r(i), "
		                      "LATERAL (VALUES (i + 1), (i)) x(v) GROUP BY i";
		const string one_state =
		    "INSERT INTO agg SELECT -1, list(v ORDER BY v) EXPORT_STATE FROM (VALUES (%s), (%s)) x(v)";
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE agg AS " + StringUtil::Format(states, "0", to_string(INITIAL_ROWS))));
		REQUIRE_NO_FAIL(con.Query("CREATE INDEX agg_first ON agg (hooked_key(id))"));
		REQUIRE_NO_FAIL(con.Query("CREATE UNIQUE INDEX agg_second ON agg ((finalize(s)[1]))"));
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO agg " + StringUtil::Format(states, to_string(INITIAL_ROWS),
		                                                                  to_string(INITIAL_ROWS + MORE_ROWS))));
		hook.Arm(con, 3);
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
		hook.Disarm();
		REQUIRE(hook.triggered.load() == 1);
		RequireRows(con, "agg", INITIAL_ROWS + MORE_ROWS);
		// every key is in the unique index
		auto last_key = INITIAL_ROWS + MORE_ROWS - 1;
		REQUIRE_FAIL(con.Query(StringUtil::Format(one_state, to_string(last_key + 1), to_string(last_key))));
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format(one_state, to_string(last_key + 2), to_string(last_key + 1))));
	}
	SECTION("while the commit removes deleted rows from the indexes") {
		REQUIRE_NO_FAIL(con.Query("BEGIN"));
		REQUIRE_NO_FAIL(con.Query("DELETE FROM t WHERE id >= 100"));
		// the removal cannot be taken back, so the commit finishes
		hook.Arm(con, 1);
		REQUIRE_NO_FAIL(con.Query("COMMIT"));
		hook.Disarm();
		REQUIRE(hook.triggered.load() == 1);
		RequireRows(con, "t", 100);
		REQUIRE_NO_FAIL(con.Query(InsertKey("t", INITIAL_ROWS / 2)));
		REQUIRE_FAIL(con.Query(InsertKey("t", 50)));
	}
}

TEST_CASE("An interrupt while the checkpoint of a commit moves rows of an index on sorted lists", "[api][index]") {
	// one thread, so that no other thread of the query sees the interrupt before the commit has finished
	auto config = GetTestConfig();
	config->SetOptionByName("threads", Value::BIGINT(1));
	DuckDB db(nullptr, config.get());
	Connection con(db);
	auto hook_ptr = CreateHookedKey(con);
	auto &hook = *hook_ptr;

	// four row groups, of which the delete leaves a quarter of the rows, so that the checkpoint merges them
	const idx_t rows = 4 * DEFAULT_ROW_GROUP_SIZE;
	REQUIRE_NO_FAIL(con.Query("SET checkpoint_threshold = '1GB'"));
	CreateIndexedTable(con, "t", rows);
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("DELETE FROM t WHERE id % 4 <> 0"));
	// the commit starts a checkpoint, which moves the rows that are left and evaluates their keys again
	REQUIRE_NO_FAIL(con.Query("SET checkpoint_threshold = '1B'"));
	// only chunks with rows that are left raise the interrupt, the removal of the deleted rows does not
	hook.Arm(con, 0);
	REQUIRE_NO_FAIL(con.Query("COMMIT"));
	hook.Disarm();
	REQUIRE(hook.triggered.load() > 0);
	auto result = con.Query("SELECT count(*), max(id) FROM t");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(NumericCast<int64_t>(rows / 4))}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::INTEGER(NumericCast<int32_t>(rows - 4))}));
	// the key of a deleted row is free, the key of a row that is left is not
	REQUIRE_NO_FAIL(con.Query(InsertKey("t", 1)));
	REQUIRE_FAIL(con.Query(InsertKey("t", 1000)));
}

TEST_CASE("An InterruptSuppressor hides the interrupts and deadlines of a database from its own thread only",
          "[api][index]") {
	auto config = GetTestConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	auto &context = *con.context;
	auto &instance = *db.instance;

	SECTION("the interrupt") {
		context.Interrupt();
		{
			InterruptSuppressor outer(instance);
			REQUIRE(context.InterruptsSuppressed());
			REQUIRE(!context.IsInterrupted());
			REQUIRE_NOTHROW(context.InterruptCheck());
#ifndef DUCKDB_NO_THREADS
			// another thread, such as one that runs a query of this client, still sees the interrupt
			bool suppressed_elsewhere = true;
			bool interrupted_elsewhere = false;
			thread other([&]() {
				suppressed_elsewhere = context.InterruptsSuppressed();
				interrupted_elsewhere = context.IsInterrupted();
			});
			other.join();
			REQUIRE(!suppressed_elsewhere);
			REQUIRE(interrupted_elsewhere);
#endif
			{
				InterruptSuppressor inner(instance);
				REQUIRE(!context.IsInterrupted());
			}
			REQUIRE(!context.IsInterrupted());
		}
		// the interrupt was not lost
		REQUIRE(!context.InterruptsSuppressed());
		REQUIRE(context.IsInterrupted());
		REQUIRE_THROWS(context.InterruptCheck());
		context.ClearInterrupt();
	}
	SECTION("the deadline") {
		// a deadline in the past, as a query that ran out of time leaves it
		context.query_deadline = 1;
		REQUIRE_THROWS(context.InterruptCheck());
		{
			InterruptSuppressor suppressor(instance);
			REQUIRE_NOTHROW(context.InterruptCheck());
		}
		REQUIRE_THROWS(context.InterruptCheck());
		context.query_deadline.SetInvalid();
	}
#ifndef DUCKDB_NO_THREADS
	SECTION("a suppressor that another thread destroys") {
		context.Interrupt();
		auto suppressor = make_uniq<InterruptSuppressor>(instance);
		REQUIRE(!context.IsInterrupted());
		// it held for the thread that created it, so that thread sees the interrupt again
		thread other([&]() { suppressor.reset(); });
		other.join();
		REQUIRE(!context.InterruptsSuppressed());
		REQUIRE(context.IsInterrupted());
		context.ClearInterrupt();
	}
#endif
	SECTION("every client of the database") {
		Connection second(db);
		auto child = Connection::CreateInternal(context);
		context.Interrupt();
		second.context->Interrupt();
		REQUIRE(child.context->IsInterrupted());
		{
			InterruptSuppressor suppressor(instance);
			REQUIRE(!context.IsInterrupted());
			REQUIRE(!second.context->IsInterrupted());
			// an internal connection reads the interrupt of its base client
			REQUIRE(!child.context->IsInterrupted());
		}
		REQUIRE(context.IsInterrupted());
		REQUIRE(second.context->IsInterrupted());
		REQUIRE(child.context->IsInterrupted());
		context.ClearInterrupt();
		second.context->ClearInterrupt();
	}
	SECTION("another database") {
		DuckDB other_db(nullptr, config.get());
		Connection other_con(other_db);
		other_con.context->Interrupt();
		InterruptSuppressor suppressor(instance);
		REQUIRE(other_con.context->IsInterrupted());
		other_con.context->ClearInterrupt();
	}
}
