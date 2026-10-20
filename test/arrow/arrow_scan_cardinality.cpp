#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"

using namespace duckdb;

namespace {

//! Serves the rows of a query result and reports the row count it was given
class CountedArrowFactory : public ArrowTestFactory {
public:
	CountedArrowFactory(Connection &con, const string &query, optional_idx row_count_p)
	    : ArrowTestFactory(vector<LogicalType>(), vector<string>(), con.context->Query(query, QueryParameters()),
	                       con.context->GetClientProperties(), *con.context),
	      row_count(row_count_p) {
		REQUIRE(!result->HasError());
		types = result->GetTypes();
		names = IdentifiersToStrings(result->GetNames());
	}

	optional_idx GetRowCount() override {
		return row_count;
	}

private:
	optional_idx row_count;
};

string Explain(Relation &rel) {
	auto explain_result = rel.Explain();
	REQUIRE(!explain_result->HasError());
	return explain_result->Collection().GetValue(1, 0).ToString();
}

} // namespace

TEST_CASE("Arrow scan estimates the row count its factory reports", "[arrow]") {
	DuckDB db;
	Connection con(db);
	// the values below 2,000,000 in a scrambled order
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE src AS SELECT (i * 7919) % 2000000 AS price FROM range(2000000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("SET profiling_renderer_settings = MAP {'operator_casing': 'upper'}"));

	auto factory = make_shared_ptr<CountedArrowFactory>(con, "SELECT price FROM src", optional_idx(2000000));
	auto top = con.TableFunction("arrow_scan", {}, {}, factory)->Order("price DESC")->Limit(10000);

	// LIMIT 10000 is below 0.7% of the rows, so a top-N beats sorting all of them, as for the table itself
	auto plan = Explain(*top);
	INFO(plan);
	REQUIRE(plan.find("TOP_N") != string::npos);
	REQUIRE(plan.find("ORDER_BY") == string::npos);
	auto scan = plan.find("ARROW_SCAN");
	REQUIRE(scan != string::npos);
	REQUIRE(plan.find("~2,000,000 rows", scan) != string::npos);
	REQUIRE(Explain(*con.Table("src")->Order("price DESC")->Limit(10000)).find("TOP_N") != string::npos);

	// the factory serves its rows once, to this query
	auto result = top->Aggregate("count(*), min(price), max(price)")->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {10000}));
	REQUIRE(CHECK_COLUMN(result, 1, {1990000}));
	REQUIRE(CHECK_COLUMN(result, 2, {1999999}));
}
