//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/table_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/function/built_in_functions.hpp"

#include "duckdb/storage/table/row_group_order_options.hpp"

namespace duckdb {
class DuckTableEntry;
class TableCatalogEntry;

//! The rows that the scan of an index creation reads
struct CreateIndexScanBoundary {
	//! The rows from this row id on are appended after the scan started
	optional_idx row_end;
	//! The reverted appends of the table when the scan started
	idx_t revert_count = 0;
};

struct TableScanBindData : public TableFunctionData {
	explicit TableScanBindData(TableCatalogEntry &table) : table(table), is_index_scan(false), is_create_index(false) {
	}

	//! The table to scan.
	TableCatalogEntry &table;
	//! The old purpose of this field has been deprecated.
	//! We now use it to express an index scan in the ANALYZE call.
	//! I.e., we const-cast the bind data and set this to true, if we opt for an index scan.
	bool is_index_scan;
	//! Whether or not the table scan is for index creation.
	bool is_create_index;
	//! In what order to scan the row groups
	unique_ptr<RowGroupOrderOptions> order_options;
	//! Subset of partition indices to scan, if null, scan all
	unique_ptr<unordered_set<idx_t>> partitions_to_scan;
	//! For the scan of an index creation, receives the rows it reads
	shared_ptr<CreateIndexScanBoundary> create_index_boundary;

public:
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<TableScanBindData>();
		return &other.table == &table;
	}
	unique_ptr<FunctionData> Copy() const override {
		auto bind_data = make_uniq<TableScanBindData>(table);
		bind_data->is_index_scan = is_index_scan;
		bind_data->is_create_index = is_create_index;
		bind_data->column_ids = column_ids;
		bind_data->order_options = order_options ? make_uniq<RowGroupOrderOptions>(*order_options) : nullptr;
		bind_data->partitions_to_scan =
		    partitions_to_scan ? make_uniq<unordered_set<idx_t>>(*partitions_to_scan) : nullptr;
		bind_data->create_index_boundary = create_index_boundary;
		return std::move(bind_data);
	}
};

//! The table scan function represents a sequential or index scan over one of DuckDB's base tables.
struct TableScanFunction {
	static void RegisterFunction(BuiltinFunctions &set);
	static TableFunction GetFunction();
};

} // namespace duckdb
