//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/catalog_scan_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class SchemaCatalogEntry;

//! Bind data of the catalog table functions that list the entries of schemas, such as duckdb_tables()
struct CatalogScanFilterData : public TableFunctionData {
	explicit CatalogScanFilterData(const vector<Identifier> &names);

	//! The columns with the database and the schema name
	optional_idx database_column;
	optional_idx schema_column;
	//! The names that filters compare these columns to, all of which a schema must have
	vector<string> database_names;
	vector<string> schema_names;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//! Records equality filters on the database_name and schema_name columns, without removing them from the plan
void CatalogScanPushdownFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data,
                               vector<unique_ptr<Expression>> &filters);

//! The visible schemas that match the recorded filters
vector<reference<SchemaCatalogEntry>> GetSchemasForScan(ClientContext &context,
                                                        optional_ptr<const FunctionData> bind_data);

} // namespace duckdb
