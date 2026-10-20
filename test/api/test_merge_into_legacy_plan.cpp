#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/planner/expression/bound_default_expression.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

using namespace duckdb;

static LogicalMergeInto &FindMergeInto(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_MERGE_INTO) {
		return op.Cast<LogicalMergeInto>();
	}
	REQUIRE(op.children.size() == 1);
	return FindMergeInto(*op.children[0]);
}

// Plans of older versions set a column to its default with a placeholder expression
TEST_CASE("A serialized MERGE plan with a default placeholder next to a computed value", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (k INTEGER, u INTEGER DEFAULT 42, s VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1, 0, 'a'), (2, 0, 'b')"));

	con.BeginTransaction();
	Parser parser(*con.context);
	parser.ParseQuery("MERGE INTO t USING (VALUES (1)) v(k) ON t.k = v.k "
	                  "WHEN MATCHED THEN UPDATE SET u = DEFAULT, s = upper(t.s)");
	Planner planner(*con.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	auto &action = *FindMergeInto(*planner.plan).actions[MergeActionCondition::WHEN_MATCHED][0];
	REQUIRE(action.columns.size() == 2);
	REQUIRE(action.columns[0].index == 1);
	action.expressions[0] = make_uniq<BoundDefaultExpression>(LogicalType::INTEGER);

	MemoryStream stream(Allocator::Get(*con.context));
	BinarySerializer::Serialize(*planner.plan, stream);
	stream.Rewind();
	bound_parameter_map_t parameters;
	auto plan = BinaryDeserializer::Deserialize<LogicalOperator>(stream, *con.context, parameters);
	plan->ResolveOperatorTypes();
	// a plan statement does not register the database it writes to
	auto &catalog = Catalog::GetCatalog(*con.context, Identifier("memory"));
	MetaTransaction::Get(*con.context).ModifyDatabase(catalog.GetAttached(), DatabaseModificationType::UPDATE_DATA);
	REQUIRE_NO_FAIL(con.Query(make_uniq<LogicalPlanStatement>(std::move(plan))));
	con.Commit();

	auto result = con.Query("SELECT u, s FROM t ORDER BY k");
	REQUIRE(CHECK_COLUMN(result, 0, {42, 0}));
	REQUIRE(CHECK_COLUMN(result, 1, {"A", "b"}));
}
