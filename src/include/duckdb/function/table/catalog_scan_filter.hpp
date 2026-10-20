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

//! A constant that a filter compares the database or schema name column to
struct CatalogNameFilter {
	string value;
	//! Whether the filter compares lower() of the column to the value
	bool lower_case = false;

	bool Matches(const string &name) const;
	bool operator==(const CatalogNameFilter &other) const;
};

//! Bind data of the catalog table functions that list the entries of schemas, such as duckdb_tables()
struct CatalogScanFilterData : public TableFunctionData {
	//! The names the database_name and schema_name columns are filtered to, all of which must match
	vector<CatalogNameFilter> database_filters;
	vector<CatalogNameFilter> schema_filters;

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
