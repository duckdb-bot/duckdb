#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

//! The EXTERNAL RESOURCE, ATTACH and CONNECT statements cannot be prepared in SQL, but the API prepares them like any
//! other statement, so a plan bound while external access was allowed can run after it was turned off
TEST_CASE("Test prepared external resource statements check external access when they run", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	// a mock resource type whose callbacks count their calls in sequences
	REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE create_calls"));
	REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE status_calls"));
	REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE destroy_calls"));
	REQUIRE_NO_FAIL(
	    con.Query("CREATE MACRO mock_create(p) AS TABLE SELECT MAP {'id': nextval('create_calls')::VARCHAR} "
	              "AS handle"));
	REQUIRE_NO_FAIL(con.Query("CREATE MACRO mock_status(h) AS TABLE SELECT CASE WHEN nextval('status_calls') > 0 THEN "
	                          "'ready' END AS state, MAP {'uri': ':memory:', 'attached_db_type': 'duckdb'} AS result"));
	REQUIRE_NO_FAIL(con.Query("CREATE MACRO mock_destroy(h) AS TABLE SELECT nextval('destroy_calls') AS status"));
	REQUIRE_NO_FAIL(
	    con.Query("CALL register_external_resource_type('mock@local', kind := 'catalog', create_function "
	              ":= 'mock_create', status_function := 'mock_status', destroy_function := 'mock_destroy')"));

	auto sequence_values = [&]() {
		auto result = con.Query("SELECT coalesce(last_value, 0) FROM duckdb_sequences() ORDER BY sequence_name");
		REQUIRE_NO_FAIL(*result);
		auto rows = result->Collection().GetRows();
		vector<int64_t> values;
		for (idx_t row = 0; row < result->RowCount(); row++) {
			values.push_back(rows.GetValue(0, row).GetValue<int64_t>());
		}
		return values;
	};

	// a registered resource, a database attached to a resource of its own, and the statements prepared while
	// external access is allowed
	REQUIRE_NO_FAIL(con.Query("CREATE EXTERNAL RESOURCE 'mock@local' AS registered"));
	REQUIRE_NO_FAIL(con.Query("ATTACH TO NEW TEMPORARY EXTERNAL RESOURCE 'mock@local' AS owned"));
	vector<string> statements = {"CREATE EXTERNAL RESOURCE 'mock@local' AS created_later",
	                             "REGISTER EXTERNAL RESOURCE 'mock@local' AS registered_later FROM {'id': 'x'}",
	                             "DESTROY EXTERNAL RESOURCE registered",
	                             "ATTACH TO NEW TEMPORARY EXTERNAL RESOURCE 'mock@local' AS owned_later",
	                             "ATTACH TO EXTERNAL RESOURCE registered AS borrowed_later",
	                             "CONNECT TO NEW TEMPORARY EXTERNAL RESOURCE 'mock@local'",
	                             "CONNECT TO EXTERNAL RESOURCE registered"};
	vector<unique_ptr<PreparedStatement>> prepared;
	for (auto &sql : statements) {
		INFO(sql);
		prepared.push_back(con.Prepare(sql));
		REQUIRE(!prepared.back()->HasError());
	}
	// create_calls, destroy_calls, status_calls
	REQUIRE(sequence_values() == vector<int64_t> {2, 0, 2});

	REQUIRE_NO_FAIL(con.Query("SET enable_external_access = false"));

	// the prepared statements keep the plan they were bound with, and are refused when they run
	for (idx_t i = 0; i < prepared.size(); i++) {
		INFO(statements[i]);
		auto result = prepared[i]->Execute();
		REQUIRE(result->HasError());
		// the binder's refusal, not wrapped into another error by the provisioning on an internal connection
		REQUIRE(result->GetErrorObject().Type() == ExceptionType::PERMISSION);
		REQUIRE(StringUtil::Contains(result->GetError(), "External resources are disabled through configuration"));
	}
	// no callback ran, the registered resource is still there and nothing was attached
	REQUIRE(sequence_values() == vector<int64_t> {2, 0, 2});
	auto resources = con.Query("SELECT name FROM duckdb_external_resources() ORDER BY name");
	REQUIRE_NO_FAIL(*resources);
	REQUIRE(resources->RowCount() == 1);
	REQUIRE(resources->Collection().GetRows().GetValue(0, 0).ToString() == "registered");
	auto databases = con.Query("SELECT count(*) FROM duckdb_databases() WHERE database_name LIKE '%later'");
	REQUIRE_NO_FAIL(*databases);
	REQUIRE(databases->Collection().GetRows().GetValue(0, 0).GetValue<int64_t>() == 0);

	// the cleanup of the resource attached while access was allowed still runs
	REQUIRE_NO_FAIL(con.Query("DETACH owned"));
	REQUIRE(sequence_values() == vector<int64_t> {2, 1, 2});
}
