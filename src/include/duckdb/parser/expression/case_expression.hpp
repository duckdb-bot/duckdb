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

class ComparisonExpression;

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
	//! The operand of a simple CASE, the left side of every comparison of its checks
	const ParsedExpression &SimpleCaseOperand() const;
	//! The comparisons of a check with the operand, one per WHEN operand, or none when the check has another form
	static vector<reference<const ComparisonExpression>> SimpleCaseComparisons(const ParsedExpression &when_expr);
	static vector<reference<ComparisonExpression>> SimpleCaseComparisonsMutable(ParsedExpression &when_expr);

public:
	string ToString() const override;

	bool Equals(const ParsedExpression &other) const override;

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
