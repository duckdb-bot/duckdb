#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/planner/expression/bound_default_expression.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

using namespace duckdb;

static LogicalMergeInto &FindMergeInto(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_MERGE_INTO) {
		return op.Cast<LogicalMergeInto>();
	}
	REQUIRE(op.children.size() == 1);
	return FindMergeInto(*op.children[0]);
}

//! Plans the MERGE of a DuckDB table and runs it, after setting the update of the first column to a placeholder
static void RunWithDefaultPlaceholder(Connection &con, const string &merge) {
	con.BeginTransaction();
	Parser parser(*con.context);
	parser.ParseQuery(merge);
	Planner planner(*con.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	auto &action = *FindMergeInto(*planner.plan).actions[MergeActionCondition::WHEN_MATCHED][0];
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
}

// Plans of older versions set a column to its default with a placeholder expression
TEST_CASE("A serialized MERGE plan with a default placeholder", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (k INTEGER, u INTEGER DEFAULT 42, s VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1, 0, 'a'), (2, 0, 'b')"));

	RunWithDefaultPlaceholder(con, "MERGE INTO t USING (VALUES (1)) v(k) ON t.k = v.k "
	                               "WHEN MATCHED THEN UPDATE SET u = DEFAULT, s = upper(t.s)");
	auto result = con.Query("SELECT u, s FROM t ORDER BY k");
	REQUIRE(CHECK_COLUMN(result, 0, {42, 0}));
	REQUIRE(CHECK_COLUMN(result, 1, {"A", "b"}));

	RunWithDefaultPlaceholder(con, "MERGE INTO t USING (VALUES (2)) v(k) ON t.k = v.k "
	                               "WHEN MATCHED THEN UPDATE SET u = DEFAULT, s = t.s");
	result = con.Query("SELECT u, s FROM t ORDER BY k");
	REQUIRE(CHECK_COLUMN(result, 0, {42, 42}));
	REQUIRE(CHECK_COLUMN(result, 1, {"A", "b"}));
}

//! A DuckDB catalog that plans MERGE INTO through the generic planning of the catalog
class GenericMergeCatalog : public DuckCatalog {
public:
	explicit GenericMergeCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

	PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner, LogicalMergeInto &op,
	                                PhysicalOperator &plan) override {
		return Catalog::PlanMergeInto(context, planner, op, plan);
	}
};

struct GenericMergeStorageExtension : StorageExtension {
	GenericMergeStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq_base<Catalog, GenericMergeCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

TEST_CASE("MERGE INTO through the generic catalog planning computes values on the rows of their action", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "generic_merge", make_shared_ptr<GenericMergeStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS g (TYPE generic_merge)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE g.t (k INTEGER, u INTEGER, s VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO g.t VALUES (1, 0, '10'), (2, 0, 'x')"));
	REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE g.seq"));

	REQUIRE_NO_FAIL(con.Query("MERGE INTO g.t USING (VALUES (1), (2), (3)) v(k) ON t.k = v.k "
	                          "WHEN MATCHED AND t.s <> 'x' THEN UPDATE SET u = t.s::INT + 1 "
	                          "WHEN MATCHED AND t.k < 0 THEN UPDATE SET u = nextval('g.seq') "
	                          "WHEN NOT MATCHED THEN INSERT VALUES (v.k, v.k * 10, 'new')"));
	auto result = con.Query("SELECT k, u, s FROM g.t ORDER BY k");
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 3}));
	REQUIRE(CHECK_COLUMN(result, 1, {11, 0, 30}));
	REQUIRE(CHECK_COLUMN(result, 2, {"10", "x", "new"}));
	result = con.Query("SELECT nextval('g.seq')");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));

	auto failed = con.Query("MERGE INTO g.t USING (VALUES (2)) v(k) ON t.k = v.k "
	                        "WHEN MATCHED THEN UPDATE SET u = t.s::INT");
	REQUIRE(failed->HasError());
	REQUIRE(StringUtil::Contains(failed->GetError(), "Could not convert string 'x' to INT32"));
}
