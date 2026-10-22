#include "duckdb/planner/operator/logical_set_operation.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
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

//! Whether a comparison of values of the type has the same result for values a set operation treats as equal
static bool ComparisonKeepsEquality(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::INTERVAL:
		return true;
	default:
		return false;
	}
}

static bool IsComparison(const Expression &expr) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		return true;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::COMPARE_BETWEEN:
	case ExpressionType::COMPARE_NOT_BETWEEN:
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN:
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL:
		return true;
	default:
		return false;
	}
}

//! Whether the expression is the collation key of a column, as ExpressionBinder::PushCollation makes it, e.g.
//! normalized_interval(i) for an INTERVAL column
static optional_ptr<const BoundColumnRefExpression> CollationKeyColumn(ClientContext &context, const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	optional_ptr<const BoundColumnRefExpression> column;
	idx_t column_count = 0;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		column = &colref;
		column_count++;
	});
	if (column_count != 1) {
		return nullptr;
	}
	unique_ptr<Expression> key = column->Copy();
	if (!ExpressionBinder::PushCollation(context, key, column->GetReturnType()) || !key->Equals(expr)) {
		return nullptr;
	}
	return column;
}

void LogicalSetOperation::ResolveChildTypes() {
	for (auto &child : children) {
		if (child->types.size() != column_count) {
			child->ResolveOperatorTypes();
		}
	}
}

bool LogicalSetOperation::FilterTellsApartEqualRows(ClientContext &context, const Expression &filter) const {
	if (filter.IsVolatile()) {
		return true;
	}
	bool tells_apart = false;
	// the types the operation compares a column with: a collation of either child applies
	auto get_compared_types = [&](const BoundColumnRefExpression &colref) {
		vector<reference<const LogicalType>> types {colref.GetReturnType()};
		auto column = colref.Binding().column_index.GetIndex();
		for (auto &child : children) {
			if (column >= child->types.size()) {
				tells_apart = true;
				continue;
			}
			types.push_back(child->types[column]);
		}
		return types;
	};
	// a column compared directly, e.g. in x > 1.5, cannot tell apart -0.0 and 0.0, and a column compared by the
	// collation key the operation compares with cannot tell apart what it treats as equal
	std::function<void(const Expression &, bool)> visit = [&](const Expression &expr, bool compared) {
		auto key_column = compared ? CollationKeyColumn(context, expr) : nullptr;
		if (key_column) {
			for (auto &type : get_compared_types(*key_column)) {
				if (!type.get().EqualsIncludingCollation(key_column->GetReturnType())) {
					tells_apart = true;
				}
			}
			return;
		}
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			auto comparison = IsComparison(expr);
			ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { visit(child, comparison); });
			return;
		}
		for (auto &type : get_compared_types(expr.Cast<BoundColumnRefExpression>())) {
			if (!EqualOnlyToItself(context, type) && !(compared && ComparisonKeepsEquality(type))) {
				tells_apart = true;
			}
		}
	};
	visit(filter, false);
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
