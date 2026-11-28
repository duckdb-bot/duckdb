#include "duckdb/parser/expression/constant_expression.hpp"

#include "duckdb/common/types/bignum_decimal.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

ConstantExpression::ConstantExpression() : ParsedExpression(ExpressionType::VALUE_CONSTANT, ExpressionClass::CONSTANT) {
}

ConstantExpression::ConstantExpression(Literal literal_p)
    : ParsedExpression(ExpressionType::VALUE_CONSTANT, ExpressionClass::CONSTANT), literal(std::move(literal_p)) {
}

string ConstantExpression::ToString() const {
	return literal.ToString();
}

Value ConstantExpression::GetValueForSerialization() const {
	auto value = literal.ToValue();
	auto &type = value.type();
	if (type.id() != LogicalTypeId::DECIMAL || !Decimal::IsWide(DecimalType::GetWidth(type))) {
		return value;
	}
	// the Value form is written for storage versions before v2.0.0, whose engines type a wider literal as DOUBLE
	const auto unscaled = value.GetValueUnsafe<bignum_t>();
	return Value::DOUBLE(DecimalHelper<bignum_t>::ToDouble(unscaled, DecimalType::GetScale(type)));
}

unique_ptr<ParsedExpression> ConstantExpression::DeserializeConstant(const Value &value, Literal literal) {
	if (literal.kind != LiteralKind::INVALID) {
		return make_uniq<ConstantExpression>(std::move(literal));
	}
	return FromValue(value);
}

} // namespace duckdb
