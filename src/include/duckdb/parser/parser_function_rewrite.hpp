//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parser_function_rewrite.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

//! Which syntactic form named the function: f(x) or x.f(y).
enum class ParserRewriteCallForm : uint8_t { PLAIN, METHOD_CHAIN };

//! Names that the parser rewrites into a syntax node instead of a catalog function call.
//! Every path that can name a function must go through this, or the paths disagree.
class ParserFunctionRewrite {
public:
	//! Whether the binder rewrites a qualified call of the (lowercase) name, as only it can tell a schema from a column
	//! the function is called on. Unpack is rewritten by the parser, as stars are expanded before binding
	static bool BinderRewritesQualifiedCall(const string &lowercase_name);
	//! Returns nullptr when the name is an ordinary function
	static unique_ptr<ParsedExpression>
	Rewrite(const string &lowercase_name, vector<FunctionArgument> &function_children, ParserRewriteCallForm call_form);
	//! Rewrites a call whose first argument is the value it is called on, as in x.f(y).
	//! Returns nullptr when the name is an ordinary function
	static unique_ptr<ParsedExpression> RewriteMethodCall(FunctionExpression &function);
};

} // namespace duckdb
