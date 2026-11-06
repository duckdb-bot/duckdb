#pragma once
#include "duckdb/parser/query_node/select_node.hpp"

namespace duckdb {

struct CreatePivotEntry {
	string enum_name;
	unique_ptr<SelectNode> base;
	unique_ptr<ParsedExpression> column;
	unique_ptr<QueryNode> subquery;
	bool has_parameters;
	//! The WITH scopes around the one of base, outermost first
	vector<CommonTableExpressionMap> outer_scopes;
};

} // namespace duckdb
