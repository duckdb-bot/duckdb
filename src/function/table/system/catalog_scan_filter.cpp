#include "duckdb/function/table/catalog_scan_filter.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

CatalogScanFilterData::CatalogScanFilterData(const vector<Identifier> &names) {
	for (idx_t i = 0; i < names.size(); i++) {
		if (names[i] == "database_name") {
			database_column = i;
		} else if (names[i] == "schema_name") {
			schema_column = i;
		}
	}
}

unique_ptr<FunctionData> CatalogScanFilterData::Copy() const {
	return make_uniq<CatalogScanFilterData>(*this);
}

bool CatalogScanFilterData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<CatalogScanFilterData>();
	return database_column == other.database_column && schema_column == other.schema_column &&
	       database_names == other.database_names && schema_names == other.schema_names;
}

void CatalogScanPushdownFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                               vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<CatalogScanFilterData>();
	auto &column_ids = get.GetColumnIds();
	for (auto &filter : filters) {
		if (!BoundComparisonExpression::IsComparison(*filter) ||
		    filter->GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
			continue;
		}
		auto &comparison = filter->Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		auto &constant_side = left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT ? left : right;
		auto &column_side = &constant_side == &left ? right : left;
		if (constant_side.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
		    column_side.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			continue;
		}
		auto &constant = constant_side.Cast<BoundConstantExpression>().GetValue();
		auto &column = column_side.Cast<BoundColumnRefExpression>();
		if (column.Depth() != 0 || column.Binding().table_index != get.table_index || constant.IsNull() ||
		    constant.type().id() != LogicalTypeId::VARCHAR) {
			continue;
		}
		auto column_index = column.Binding().column_index.GetIndex();
		if (column_index >= column_ids.size() || column_ids[column_index].IsVirtualColumn()) {
			continue;
		}
		auto primary_index = column_ids[column_index].GetPrimaryIndex();
		if (bind_data.database_column == primary_index) {
			bind_data.database_names.push_back(StringValue::Get(constant));
		} else if (bind_data.schema_column == primary_index) {
			bind_data.schema_names.push_back(StringValue::Get(constant));
		}
	}
}

static bool MatchesAll(const vector<string> &names, const string &name) {
	for (auto &filter_name : names) {
		if (filter_name != name) {
			return false;
		}
	}
	return true;
}

vector<reference<SchemaCatalogEntry>> GetSchemasForScan(ClientContext &context,
                                                        optional_ptr<const FunctionData> bind_data_p) {
	if (!bind_data_p) {
		return Catalog::GetAllSchemas(context);
	}
	auto &bind_data = bind_data_p->Cast<CatalogScanFilterData>();
	return Catalog::GetAllSchemas(
	    context,
	    [&](Catalog &catalog) { return MatchesAll(bind_data.database_names, catalog.GetName().GetIdentifierName()); },
	    [&](SchemaCatalogEntry &schema) {
		    return MatchesAll(bind_data.schema_names, schema.name.GetIdentifierName());
	    });
}

} // namespace duckdb
