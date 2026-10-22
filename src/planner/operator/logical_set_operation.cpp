#include "duckdb/planner/operator/logical_set_operation.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

LogicalSetOperation::LogicalSetOperation(TableIndex table_index, idx_t column_count, LogicalOperatorType type,
                                         bool setop_all, bool allow_out_of_order)
    : LogicalOperator(type), table_index(table_index), column_count(column_count), setop_all(setop_all),
      allow_out_of_order(allow_out_of_order) {
}

LogicalSetOperation::LogicalSetOperation(TableIndex table_index, idx_t column_count,
                                         vector<unique_ptr<LogicalOperator>> children_p, LogicalOperatorType type,
                                         bool setop_all, bool allow_out_of_order)
    : LogicalOperator(type), table_index(table_index), column_count(column_count), setop_all(setop_all),
      allow_out_of_order(allow_out_of_order) {
	D_ASSERT(type == LogicalOperatorType::LOGICAL_UNION || type == LogicalOperatorType::LOGICAL_EXCEPT ||
	         type == LogicalOperatorType::LOGICAL_INTERSECT);
	children = std::move(children_p);
}

LogicalSetOperation::LogicalSetOperation(TableIndex table_index, idx_t column_count, unique_ptr<LogicalOperator> top,
                                         unique_ptr<LogicalOperator> bottom, LogicalOperatorType type, bool setop_all,
                                         bool allow_out_of_order)
    : LogicalOperator(type), table_index(table_index), column_count(column_count), setop_all(setop_all),
      allow_out_of_order(allow_out_of_order) {
	D_ASSERT(type == LogicalOperatorType::LOGICAL_UNION || type == LogicalOperatorType::LOGICAL_EXCEPT ||
	         type == LogicalOperatorType::LOGICAL_INTERSECT);
	children.push_back(std::move(top));
	children.push_back(std::move(bottom));
}

vector<TableIndex> LogicalSetOperation::GetTableIndex() const {
	return vector<TableIndex> {table_index};
}

//! Whether a value of the type is only equal to itself in a set operation
static bool EqualOnlyToItself(ClientContext &context, const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::ENUM:
		return true;
	case LogicalTypeId::VARCHAR: {
		// a collation, also the default collation, makes other strings equal
		unique_ptr<Expression> expr = make_uniq<BoundReferenceExpression>(type, 0ULL);
		return !ExpressionBinder::PushCollation(context, expr, type);
	}
	default:
		// e.g. -0.0 and 0.0, INTERVAL '30 days' and INTERVAL '1 month', and nested types that hold such values
		return false;
	}
}

bool LogicalSetOperation::FilterTellsApartEqualRows(ClientContext &context, const Expression &filter) const {
	if (filter.IsVolatile()) {
		return true;
	}
	bool tells_apart = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(filter, [&](const BoundColumnRefExpression &colref) {
		if (!EqualOnlyToItself(context, colref.GetReturnType())) {
			tells_apart = true;
			return;
		}
		// the set operation compares with the collation of either child
		auto column = colref.Binding().column_index.GetIndex();
		for (auto &child : children) {
			if (column < child->types.size() && !EqualOnlyToItself(context, child->types[column])) {
				tells_apart = true;
			}
		}
	});
	return tells_apart;
}

string LogicalSetOperation::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return LogicalOperator::GetName() + StringUtil::Format(" #%llu", table_index.index);
	}
#endif
	return LogicalOperator::GetName();
}

} // namespace duckdb
