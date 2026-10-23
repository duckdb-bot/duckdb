#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "test_helpers.hpp"

#include "duckdb.hpp"

using namespace duckdb;

TEST_CASE("Loaded tables keep their column definitions compact", "[catalog]") {
	// most columns have no comment or tags, so a column definition does not hold room for them
	REQUIRE(sizeof(ColumnDefinition) < 128);

	auto db_path = TestCreatePath("table_metadata_memory.db");
	DeleteDatabase(db_path);
	auto config = GetTestConfig();
	{
		DuckDB db(db_path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (c0 INT, c1 INT, c2 INT, c3 INT, c4 INT, c5 INT, c6 INT, c7 INT, "
		                          "c8 INT, c9 INT)"));
		REQUIRE_NO_FAIL(con.Query("COMMENT ON COLUMN t.c3 IS 'third'"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	DuckDB db(db_path, config.get());
	Connection con(db);
	con.BeginTransaction();
	auto &table =
	    Catalog::GetEntry<TableCatalogEntry>(*con.context, QualifiedName(INVALID_CATALOG, DEFAULT_SCHEMA, "t"));
	// the storage of a loaded table holds exactly its columns
	REQUIRE(table.GetStorage().Columns().size() == 10);
	REQUIRE(table.GetStorage().Columns().capacity() == 10);
	con.Commit();

	auto result = con.Query("SELECT column_name, comment FROM duckdb_columns() WHERE table_name = 't' AND comment "
	                        "IS NOT NULL");
	REQUIRE(CHECK_COLUMN(result, 0, {"c3"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"third"}));
	DeleteDatabase(db_path);
}

static ColumnDefinition RoundTrip(const ColumnDefinition &column) {
	MemoryStream stream(Allocator::DefaultAllocator());
	BinarySerializer::Serialize(column, stream);
	stream.Rewind();
	BinaryDeserializer deserializer(stream);
	deserializer.Begin();
	auto result = ColumnDefinition::Deserialize(deserializer);
	deserializer.End();
	return result;
}

TEST_CASE("Column comments and tags survive copies and serialization", "[catalog]") {
	ColumnDefinition plain("c", LogicalType::INTEGER);
	REQUIRE(plain.Comment().IsNull());
	REQUIRE(plain.Comment().type().id() == LogicalTypeId::SQLNULL);
	REQUIRE(plain.Tags().empty());
	auto plain_copy = RoundTrip(plain);
	REQUIRE(plain_copy.Comment().IsNull());
	REQUIRE(plain_copy.Tags().empty());

	// a NULL comment of a type other than SQLNULL keeps its type
	ColumnDefinition typed_null("c", LogicalType::INTEGER);
	typed_null.SetComment(Value(LogicalType::VARCHAR));
	REQUIRE(typed_null.Comment().IsNull());
	REQUIRE(typed_null.Comment().type().id() == LogicalTypeId::VARCHAR);
	REQUIRE(typed_null.Copy().Comment().type().id() == LogicalTypeId::VARCHAR);

	ColumnDefinition annotated("c", LogicalType::INTEGER);
	annotated.SetComment(Value("a comment"));
	InsertionOrderPreservingMap<string> tags;
	tags["second"] = "2";
	tags["first"] = "1";
	annotated.SetTags(tags);

	// a copy holds annotations of its own
	auto copy = annotated.Copy();
	copy.SetComment(Value("another comment"));
	InsertionOrderPreservingMap<string> other_tags;
	other_tags["third"] = "3";
	copy.SetTags(other_tags);
	REQUIRE(annotated.Comment() == Value("a comment"));
	REQUIRE(annotated.Tags().size() == 2);

	auto column = RoundTrip(annotated);
	REQUIRE(column.Comment() == Value("a comment"));
	REQUIRE(column.Tags().size() == 2);
	auto it = column.Tags().begin();
	REQUIRE(it->first == "second");
	REQUIRE(it->second == "2");
	++it;
	REQUIRE(it->first == "first");
	REQUIRE(it->second == "1");

	// a moved column keeps the annotations
	auto moved = std::move(copy);
	REQUIRE(moved.Comment() == Value("another comment"));
	REQUIRE(moved.Tags().size() == 1);
}
