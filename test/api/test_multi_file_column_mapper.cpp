#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_column_mapper.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

using namespace duckdb;

namespace {
class MappingTestReader : public BaseFileReader {
public:
	MappingTestReader() : BaseFileReader(OpenFileInfo("mapping-test")) {
	}
	bool TryInitializeScan(ClientContext &, GlobalTableFunctionState &, LocalTableFunctionState &) override {
		throw InternalException("Mapping test does not scan");
	}
	AsyncResult Scan(ClientContext &, GlobalTableFunctionState &, LocalTableFunctionState &, DataChunk &) override {
		throw InternalException("Mapping test does not scan");
	}
	string GetReaderType() const override {
		return "mapping test";
	}
};

MultiFileColumnDefinition Field(const string &name, const LogicalType &type, int32_t id) {
	MultiFileColumnDefinition result(name, type);
	result.identifier = Value::INTEGER(id);
	return result;
}
} // namespace

TEST_CASE("Field-id mapping requires a match or an explicit default", "[multifile]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto reader = make_shared_ptr<MappingTestReader>();
	MultiFileReaderData reader_data(reader);
	MultiFileReader multi_file_reader;
	SimpleMultiFileList files({OpenFileInfo("mapping-test")});
	vector<MultiFileColumnDefinition> columns;
	columns.push_back(Field("required_field", LogicalType::INTEGER, 42));
	vector<ColumnIndex> indexes {ColumnIndex(0)};
	virtual_column_map_t virtual_columns;
	MultiFileColumnMapper mapper(*con.context, multi_file_reader, reader_data, columns, indexes, nullptr, files,
	                             virtual_columns);

	SECTION("Missing field without a default raises a recoverable error") {
		REQUIRE_THROWS_AS(mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID), InvalidInputException);
		REQUIRE_THROWS_WITH(
		    mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID),
		    Catch::Contains("(field id 42) is missing from the file schema and has no default expression"));
	}
	SECTION("Matching field does not need a default") {
		reader->columns.push_back(Field("file_field", LogicalType::INTEGER, 42));
		REQUIRE(mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID) == ReaderInitializeType::INITIALIZED);
		REQUIRE(reader->column_ids.size() == 1);
	}
	SECTION("Explicit NULL and non-NULL defaults are accepted") {
		auto value = GENERATE(Value(LogicalType::INTEGER), Value::INTEGER(7));
		columns[0].default_expression = ConstantExpression::FromValue(value);
		REQUIRE(mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID) == ReaderInitializeType::INITIALIZED);
		REQUIRE(reader_data.expressions.size() == 1);
		REQUIRE(ValueOperations::NotDistinctFrom(reader_data.expressions[0]->Cast<BoundConstantExpression>().GetValue(),
		                                         value));
	}
	SECTION("Missing MAP key without a default raises a recoverable error") {
		auto map_type = LogicalType::MAP(LogicalType::INTEGER, LogicalType::INTEGER);
		columns[0] = Field("m", map_type, 1);
		columns[0].children.push_back(Field("key", LogicalType::INTEGER, 42));
		columns[0].children.push_back(Field("value", LogicalType::INTEGER, 43));
		auto local_map = Field("m", map_type, 1);
		MultiFileColumnDefinition key_value("key_value", ListType::GetChildType(map_type));
		key_value.children.push_back(Field("key", LogicalType::INTEGER, 99));
		key_value.children.push_back(Field("value", LogicalType::INTEGER, 43));
		local_map.children.push_back(key_value);
		reader->columns.push_back(local_map);
		REQUIRE_THROWS_AS(mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID), InvalidInputException);
		REQUIRE_THROWS_WITH(
		    mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID),
		    Catch::Contains("(field id 42) is missing from the file schema and has no default expression"));
	}
	con.Rollback();
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
}

namespace {
//! A registered cast that marks every value it converts, so a result tells whether it ran instead of the default cast
bool MarkDecimalAsDouble(Vector &source, Vector &result, idx_t count, CastParameters &) {
	auto writer = FlatVector::Writer<double>(result, count);
	for (auto entry : source.Values<int16_t>()) {
		if (entry.IsValid()) {
			writer.WriteValue(42.0);
		} else {
			writer.WriteNull();
		}
	}
	return true;
}
} // namespace

TEST_CASE("A file column whose text becomes bytes keeps the registered casts of its other fields", "[multifile]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto parquet = con.Query("SELECT 1 FROM duckdb_extensions() WHERE extension_name = 'parquet' AND loaded");
	if (parquet->HasError() || parquet->RowCount() == 0) {
		return;
	}
	auto text_file = TestCreatePath("text_as_bytes_text.parquet");
	auto bytes_file = TestCreatePath("text_as_bytes_bytes.parquet");
	REQUIRE_NO_FAIL(
	    con.Query("COPY (SELECT 1 AS i, {'a': 'a\\x41', 'n': 1.5::DECIMAL(4,1)} AS s) TO '" + text_file + "'"));
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT 2 AS i, {'a': 'b'::BLOB, 'n': 2.5::DOUBLE} AS s) TO '" + bytes_file + "'"));
	// from here on this database casts a DECIMAL(4,1) to DOUBLE with the registered cast, not the default one
	CastFunctionSet::Get(*con.context)
	    .RegisterCastFunction(LogicalType::DECIMAL(4, 1), LogicalType::DOUBLE, BoundCastInfo(MarkDecimalAsDouble));
	// the whole struct is read, so its fields are cast together, the text of the first file as its five bytes and its
	// decimal through the registered cast
	auto result = con.Query("SELECT i, octet_length(s.a), s.n, s FROM read_parquet(['" + text_file + "', '" +
	                        bytes_file + "'], union_by_name = true) ORDER BY i");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2}));
	REQUIRE(CHECK_COLUMN(result, 1, {5, 1}));
	REQUIRE(CHECK_COLUMN(result, 2, {42.0, 2.5}));

	// the BLOB type of an extension carries an alias, the text becomes its bytes all the same
	ExtensionLoader loader(*db.instance, "text_as_bytes_test");
	loader.RegisterType("byte_alias", LogicalType(LogicalType::BLOB).WithAlias("byte_alias"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE aliased (i INTEGER, s STRUCT(a byte_alias, n DOUBLE))"));
	REQUIRE_NO_FAIL(con.Query("COPY aliased FROM '" + text_file + "'"));
	result = con.Query("SELECT i, s.n, s FROM aliased");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(CHECK_COLUMN(result, 1, {42.0}));
	auto aliased_text = StructValue::GetChildren(result->Collection().GetRows().GetValue(2, 0))[0];
	REQUIRE(aliased_text.type().id() == LogicalTypeId::BLOB);
	REQUIRE(StringValue::Get(aliased_text) == "a\\x41");
}
