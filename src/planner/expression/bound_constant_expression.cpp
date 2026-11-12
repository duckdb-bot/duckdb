#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

BoundConstantExpression::BoundConstantExpression(Value value_p)
    : Expression(ExpressionType::VALUE_CONSTANT, ExpressionClass::BOUND_CONSTANT, value_p.type()),
      value(std::move(value_p)) {
}

BoundConstantExpression::BoundConstantExpression(Value value_p, Identifier alias_p, QueryLocation location)
    : BoundConstantExpression(std::move(value_p)) {
	alias = std::move(alias_p);
	query_location = location;
}

unique_ptr<Expression> BoundConstantExpression::WithValue(Value new_value) const {
	return make_uniq<BoundConstantExpression>(std::move(new_value), alias, query_location);
}

void BoundConstantExpression::SetReturnType(LogicalType type) {
	D_ASSERT(value.type() == type);
	value = value.WithType(type);
	return_type = std::move(type);
}

string BoundConstantExpression::ToString() const {
	return value.ToSQLString();
}

static bool SameRepresentation(const Value &left, const Value &right);

static bool SameChildren(const vector<Value> &left, const vector<Value> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (idx_t i = 0; i < left.size(); i++) {
		if (!SameRepresentation(left[i], right[i])) {
			return false;
		}
	}
	return true;
}

//! Two constants are the same expression only if their values have the same representation, not just compare equal
static bool SameRepresentation(const Value &left, const Value &right) {
	if (left.IsNull() || right.IsNull()) {
		return left.IsNull() && right.IsNull();
	}
	if (left.type().id() == LogicalTypeId::VARIANT) {
		// values compare a VARIANT by meaning, vectors compare the STRUCT it is stored as
		Vector left_vector(left, count_t(1));
		Vector right_vector(right, count_t(1));
		return VectorOperations::NotDistinctFrom(left_vector, right_vector, nullptr, 1, nullptr, nullptr) == 1;
	}
	switch (left.type().InternalType()) {
	case PhysicalType::INTERVAL:
		// INTERVAL 1 MONTH and INTERVAL 30 DAY are equal after normalization but add differently
		return left.GetValueUnsafe<interval_t>().SameFields(right.GetValueUnsafe<interval_t>());
	case PhysicalType::FLOAT: {
		auto left_float = left.GetValueUnsafe<float>();
		auto right_float = right.GetValueUnsafe<float>();
		return memcmp(&left_float, &right_float, sizeof(float)) == 0;
	}
	case PhysicalType::DOUBLE: {
		auto left_double = left.GetValueUnsafe<double>();
		auto right_double = right.GetValueUnsafe<double>();
		return memcmp(&left_double, &right_double, sizeof(double)) == 0;
	}
	case PhysicalType::LIST:
		return SameChildren(ListValue::GetChildren(left), ListValue::GetChildren(right));
	case PhysicalType::ARRAY:
		return SameChildren(ArrayValue::GetChildren(left), ArrayValue::GetChildren(right));
	case PhysicalType::STRUCT:
		return SameChildren(StructValue::GetChildren(left), StructValue::GetChildren(right));
	default:
		return !ValueOperations::DistinctFrom(left, right);
	}
}

bool BoundConstantExpression::Equals(const BaseExpression &other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BoundConstantExpression>();
	if (value.type() != other.value.type()) {
		return false;
	}
	return SameRepresentation(value, other.value);
}

hash_t BoundConstantExpression::Hash() const {
	hash_t result = Expression::Hash();
	if (value.type().id() == LogicalTypeId::INTERVAL && !value.IsNull()) {
		// Hash by the raw (months, days, micros) fields: Value::Hash normalizes first, which would put intervals
		// that are only equal after normalization in the same bucket and make Equals the deciding factor
		auto interval = value.GetValueUnsafe<interval_t>();
		return CombineHash(duckdb::Hash(interval.months) ^ duckdb::Hash(interval.days) ^ duckdb::Hash(interval.micros),
		                   result);
	}
	return CombineHash(value.Hash(), result);
}

unique_ptr<Expression> BoundConstantExpression::Copy() const {
	auto copy = make_uniq<BoundConstantExpression>(value);
	copy->CopyProperties(*this);
	return std::move(copy);
}

} // namespace duckdb
