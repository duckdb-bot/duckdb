#include "duckdb/function/table/catalog_scan_filter.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
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

static vector<unique_ptr<Expression>> CopyFilters(const vector<unique_ptr<Expression>> &filters) {
	vector<unique_ptr<Expression>> result;
	for (auto &filter : filters) {
		result.push_back(filter->Copy());
	}
	return result;
}

unique_ptr<FunctionData> CatalogScanFilterData::Copy() const {
	auto result = make_uniq<CatalogScanFilterData>(vector<Identifier>());
	result->database_column = database_column;
	result->schema_column = schema_column;
	result->database_filters = CopyFilters(database_filters);
	result->schema_filters = CopyFilters(schema_filters);
	return std::move(result);
}

bool CatalogScanFilterData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<CatalogScanFilterData>();
	return database_column == other.database_column && schema_column == other.schema_column &&
	       Expression::ListEquals(database_filters, other.database_filters) &&
	       Expression::ListEquals(schema_filters, other.schema_filters);
}

//! Replaces the name columns with references to the names, false if the filter reads anything else
static bool ReferenceNames(unique_ptr<Expression> &expr, const LogicalGet &get, const CatalogScanFilterData &bind_data,
                           bool &reads_name, bool &reads_schema) {
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto &column = expr->Cast<BoundColumnRefExpression>();
		auto &column_ids = get.GetColumnIds();
		auto column_index = column.Binding().column_index.GetIndex();
		if (column.Depth() != 0 || column.Binding().table_index != get.table_index ||
		    column_index >= column_ids.size() || column_ids[column_index].IsVirtualColumn()) {
			return false;
		}
		auto primary_index = column_ids[column_index].GetPrimaryIndex();
		if (bind_data.database_column != primary_index && bind_data.schema_column != primary_index) {
			return false;
		}
		bool is_schema = bind_data.schema_column == primary_index;
		reads_name = true;
		reads_schema = reads_schema || is_schema;
		expr = make_uniq<BoundReferenceExpression>(expr->GetReturnType(), is_schema ? 1 : 0);
		return true;
	}
	case ExpressionClass::BOUND_CONSTANT:
		return true;
	case ExpressionClass::BOUND_FUNCTION:
	case ExpressionClass::BOUND_CONJUNCTION:
	case ExpressionClass::BOUND_OPERATOR:
	case ExpressionClass::BOUND_CASE: {
		bool success = true;
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
			success = success && ReferenceNames(child, get, bind_data, reads_name, reads_schema);
		});
		return success;
	}
	default:
		return false;
	}
}

void CatalogScanPushdownFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                               vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<CatalogScanFilterData>();
	for (auto &filter : filters) {
		// pruning would change the rows that volatile or throwing filters run on
		if (filter->IsVolatile() || filter->CanThrow()) {
			return;
		}
	}
	for (auto &filter : filters) {
		auto name_filter = filter->Copy();
		bool reads_name = false;
		bool reads_schema = false;
		if (!ReferenceNames(name_filter, get, bind_data, reads_name, reads_schema) || !reads_name) {
			continue;
		}
		auto &target = reads_schema ? bind_data.schema_filters : bind_data.database_filters;
		target.push_back(std::move(name_filter));
	}
}

namespace {

//! Runs recorded filters on a database name and a schema name
class CatalogNameMatcher {
public:
	CatalogNameMatcher(ClientContext &context, const vector<unique_ptr<Expression>> &filters) {
		for (auto &filter : filters) {
			executors.push_back(make_uniq<ExpressionExecutor>(context, *filter));
		}
		names.Initialize(context, {LogicalType::VARCHAR, LogicalType::VARCHAR}, 1);
	}

	bool Matches(const Identifier &database_name, const Identifier &schema_name) {
		if (executors.empty() || failed) {
			return true;
		}
		names.Reset();
		auto database_name_str = database_name.GetIdentifierName();
		auto schema_name_str = schema_name.GetIdentifierName();
		FlatVector::Writer<string_t>(names.data[0], 1).WriteValue(string_t(database_name_str));
		FlatVector::Writer<string_t>(names.data[1], 1).WriteValue(string_t(schema_name_str));
		names.CheckCardinality(1);
		SelectionVector sel(1);
		bool matches = true;
		// every filter runs, so that a filter that fails on these names is found
		for (auto &executor : executors) {
			try {
				matches = executor->SelectExpression(names, sel) > 0 && matches;
			} catch (InternalException &ex) {
				throw;
			} catch (...) {
				failed = true;
				return true;
			}
		}
		return matches;
	}

	//! Whether a filter failed on some names, then it may fail on rows that a pruned scan would not produce
	bool failed = false;

private:
	vector<unique_ptr<ExpressionExecutor>> executors;
	DataChunk names;
};

} // namespace

vector<reference<SchemaCatalogEntry>> GetSchemasForScan(ClientContext &context,
                                                        optional_ptr<const FunctionData> bind_data_p) {
	if (!bind_data_p) {
		return Catalog::GetAllSchemas(context);
	}
	auto &bind_data = bind_data_p->Cast<CatalogScanFilterData>();
	CatalogNameMatcher database_matcher(context, bind_data.database_filters);
	CatalogNameMatcher schema_matcher(context, bind_data.schema_filters);
	auto schemas = Catalog::GetAllSchemas(
	    context, [&](Catalog &catalog) { return database_matcher.Matches(catalog.GetName(), Identifier()); },
	    [&](SchemaCatalogEntry &schema) { return schema_matcher.Matches(schema.catalog.GetName(), schema.name); });
	if (database_matcher.failed || schema_matcher.failed) {
		return Catalog::GetAllSchemas(context);
	}
	return schemas;
}

} // namespace duckdb
