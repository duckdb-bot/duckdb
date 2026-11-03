//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parser_function_rewrite.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
class FunctionExpression;

//! The names the parser rewrites into a syntax node instead of a catalog function call (ifnull, coalesce, try,
//! construct_array, unpack and date). A call written as x.f(y) parses as a call in schema x, and only the binder can
//! tell a schema holding such a function from the column x the function is called on, so it applies the rewrite then
class ParserFunctionRewrite {
public:
	//! Whether the parser rewrites a function of this name
	static bool Rewrites(const Identifier &function_name);
	//! Whether RewriteMethodCall rewrites the call, which is an unqualified call of such a name
	static bool RewritesCall(const FunctionExpression &function);
	//! Rewrites an unqualified call of such a name, whose first argument is the value it is called on as in x.f(y),
	//! into its syntax node. Returns nullptr for any other call
	static unique_ptr<ParsedExpression> RewriteMethodCall(FunctionExpression &function);
};

} // namespace duckdb
