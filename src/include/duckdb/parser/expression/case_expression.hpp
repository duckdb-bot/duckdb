//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression/case_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

struct CaseCheck {
	unique_ptr<ParsedExpression> when_expr;
	unique_ptr<ParsedExpression> then_expr;

	void Serialize(Serializer &serializer) const;
	static CaseCheck Deserialize(Deserializer &deserializer);
};

//! The CaseExpression represents a CASE expression in the query
class CaseExpression : public ParsedExpression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::CASE;

public:
	DUCKDB_API CaseExpression();

public:
	const vector<CaseCheck> &CaseChecks() const {
		return case_checks;
	}
	const ParsedExpression &Else() const {
		return *else_expr;
	}
	vector<CaseCheck> &CaseChecksMutable() {
		return case_checks;
	}
	unique_ptr<ParsedExpression> &ElseMutable() {
		return else_expr;
	}
	//! Whether this is a simple CASE (CASE operand WHEN value ...), whose checks compare copies of the operand
	bool IsSimpleCase() const {
		return simple_case;
	}
	bool &SimpleCaseMutable() {
		return simple_case;
	}
	//! Whether this is a simple CASE whose checks still all compare its operand, as the parser built them
	bool HasSimpleCaseChecks() const;
	//! The operand of a simple CASE, the left side of every check
	const ParsedExpression &SimpleCaseOperand() const;

public:
	string ToString() const override;

	bool Equals(const ParsedExpression &other) const override;
	hash_t Hash() const override;

	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);

public:
	template <class T, class BASE>
	static string ToString(const T &entry) {
		string case_str = "CASE ";
		for (auto &check : entry.CaseChecks()) {
			case_str += " WHEN (" + check.when_expr->ToString() + ")";
			case_str += " THEN (" + check.then_expr->ToString() + ")";
		}
		case_str += " ELSE " + entry.Else().ToString();
		case_str += " END";
		return case_str;
	}

private:
	vector<CaseCheck> case_checks;
	unique_ptr<ParsedExpression> else_expr;
	bool simple_case = false;
};
} // namespace duckdb
