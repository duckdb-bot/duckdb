#include "duckdb/function/table/catalog_scan_filter.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

static string LowerName(const string &name) {
	auto length = LowerLength(name.c_str(), name.size());
	string result(length, '\0');
	LowerCase(name.c_str(), name.size(), &result[0]);
	return result;
}

bool CatalogNameFilter::Matches(const string &name) const {
	return (lower_case ? LowerName(name) : name) == value;
}

bool CatalogNameFilter::operator==(const CatalogNameFilter &other) const {
	return value == other.value && lower_case == other.lower_case;
}

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
	       database_filters == other.database_filters && schema_filters == other.schema_filters;
}

//! Whether the function is the built-in lower(), another function can have its name and callbacks
static bool IsBuiltinLower(const BoundScalarFunction &function) {
	using callback_t = void (*)(DataChunk &, ExpressionState &, Vector &);
	auto builtin = LowerFun::GetFunction();
	auto builtin_callback = builtin.GetFunctionCallback();
	auto callback = function.GetFunctionCallback();
	auto builtin_target = builtin_callback.target<callback_t>();
	auto target = callback.target<callback_t>();
	return function.GetCallbacks() == builtin.GetCallbacks() && builtin_target && target && *builtin_target == *target;
}

//! The column that the expression is, or the built-in lower() of
static optional_ptr<const BoundColumnRefExpression> GetFilteredColumn(const Expression &expr, bool &lower_case) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		lower_case = false;
		return expr.Cast<BoundColumnRefExpression>();
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	auto &children = function.GetChildren();
	if (!IsBuiltinLower(function.Function()) || children.size() != 1 ||
	    children[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	lower_case = true;
	return children[0]->Cast<BoundColumnRefExpression>();
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
		if (constant_side.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			continue;
		}
		auto &constant = constant_side.Cast<BoundConstantExpression>().GetValue();
		bool lower_case;
		auto column = GetFilteredColumn(column_side, lower_case);
		if (!column || column->Depth() != 0 || column->Binding().table_index != get.table_index || constant.IsNull() ||
		    constant.type().id() != LogicalTypeId::VARCHAR) {
			continue;
		}
		auto column_index = column->Binding().column_index.GetIndex();
		if (column_index >= column_ids.size() || column_ids[column_index].IsVirtualColumn()) {
			continue;
		}
		auto primary_index = column_ids[column_index].GetPrimaryIndex();
		CatalogNameFilter name_filter {StringValue::Get(constant), lower_case};
		if (bind_data.database_column == primary_index) {
			bind_data.database_filters.push_back(std::move(name_filter));
		} else if (bind_data.schema_column == primary_index) {
			bind_data.schema_filters.push_back(std::move(name_filter));
		}
	}
}

static bool MatchesAll(const vector<CatalogNameFilter> &name_filters, const string &name) {
	for (auto &name_filter : name_filters) {
		if (!name_filter.Matches(name)) {
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
	    [&](Catalog &catalog) { return MatchesAll(bind_data.database_filters, catalog.GetName().GetIdentifierName()); },
	    [&](SchemaCatalogEntry &schema) {
		    return MatchesAll(bind_data.schema_filters, schema.name.GetIdentifierName());
	    });
}

} // namespace duckdb
