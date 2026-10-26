#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

BoundConstantExpression::BoundConstantExpression(Value value_p)
    : Expression(ExpressionType::VALUE_CONSTANT, ExpressionClass::BOUND_CONSTANT, value_p.type()),
      value(std::move(value_p)) {
}

void BoundConstantExpression::SetValue(Value value_p) {
	return_type = value_p.type();
	value = std::move(value_p);
}

Value BoundConstantExpression::TakeValue() {
	return std::move(value);
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
	case PhysicalType::INTERVAL: {
		auto left_interval = left.GetValueUnsafe<interval_t>();
		auto right_interval = right.GetValueUnsafe<interval_t>();
		return left_interval.months == right_interval.months && left_interval.days == right_interval.days &&
		       left_interval.micros == right_interval.micros;
	}
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
	return value.type() == other.value.type() && SameRepresentation(value, other.value);
}

hash_t BoundConstantExpression::Hash() const {
	hash_t result = Expression::Hash();
	return CombineHash(value.Hash(), result);
}

unique_ptr<Expression> BoundConstantExpression::Copy() const {
	auto copy = make_uniq<BoundConstantExpression>(value);
	copy->CopyProperties(*this);
	return std::move(copy);
}

} // namespace duckdb
