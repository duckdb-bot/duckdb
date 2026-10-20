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
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class SchemaCatalogEntry;

//! Bind data of the catalog table functions that list the entries of schemas, such as duckdb_tables()
struct CatalogScanFilterData : public TableFunctionData {
	explicit CatalogScanFilterData(const vector<Identifier> &names);

	//! The columns with the database and the schema name
	optional_idx database_column;
	optional_idx schema_column;
	//! Filters that only read the database name, and filters that read the schema name
	//! The database name is reference 0 and the schema name reference 1
	vector<unique_ptr<Expression>> database_filters;
	vector<unique_ptr<Expression>> schema_filters;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//! Records the filters that only read the database and schema name columns, without removing them from the plan
void CatalogScanPushdownFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data,
                               vector<unique_ptr<Expression>> &filters);

//! The visible schemas that match the recorded filters
vector<reference<SchemaCatalogEntry>> GetSchemasForScan(ClientContext &context,
                                                        optional_ptr<const FunctionData> bind_data);

} // namespace duckdb
