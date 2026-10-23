#include "duckdb/planner/expression_binder.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_qualifier.hpp"

namespace duckdb {

idx_t ExpressionBinder::ScopeCount() const {
	return binder.GetEnclosingScopes().size() + 1;
}

ExpressionBinder &ExpressionBinder::ScopeAt(idx_t depth) {
	D_ASSERT(depth < ScopeCount());
	if (depth == 0) {
		return *this;
	}
	// the enclosing scopes are stored outermost first, so the index is counted from the back
	auto &enclosing = binder.GetEnclosingScopes();
	return enclosing[enclosing.size() - depth];
}

static QueryLocation ExtractLocation(const unordered_map<string, string> &info) {
	auto pos_entry = info.find("position");
	if (pos_entry == info.end()) {
		return QueryLocation();
	}
	uint64_t start;
	if (!TryCast::Operation<string_t, uint64_t>(string_t(pos_entry->second), start)) {
		return QueryLocation();
	}
	uint64_t length = 0;
	auto location_entry = info.find("location");
	if (location_entry != info.end()) {
		// value is formatted as "[start,length]"
		auto comma = location_entry->second.find(',');
		if (comma != string::npos) {
			auto len_str = location_entry->second.substr(comma + 1);
			if (!len_str.empty() && len_str.back() == ']') {
				len_str.pop_back();
			}
			TryCast::Operation<string_t, uint64_t>(string_t(len_str), length);
		}
	}
	return QueryLocation(start, length);
}

static bool CombineMissingColumns(ErrorData &current, ErrorData new_error) {
	auto &current_info = current.ExtraInfo();
	auto &new_info = new_error.ExtraInfo();
	auto current_entry = current_info.find("error_subtype");
	auto new_entry = new_info.find("error_subtype");
	if (current_entry == current_info.end() || new_entry == new_info.end()) {
		// no subtype info in either expression
		return false;
	}
	if (current_entry->second != "COLUMN_NOT_FOUND" || new_entry->second != "COLUMN_NOT_FOUND") {
		// either info is not a `COLUMN_NOT_FOUND`
		return false;
	}
	current_entry = current_info.find("name");
	new_entry = new_info.find("name");
	if (current_entry == current_info.end() || new_entry == new_info.end()) {
		// no candidate info in either column
		return false;
	}
	if (current_entry->second != new_entry->second) {
		// error does not concern the same name/column
		return false;
	}
	auto column_name = current_entry->second;
	current_entry = current_info.find("candidates");
	new_entry = new_info.find("candidates");
	if (current_entry == current_info.end()) {
		// no current candidates - use new candidates
		current = std::move(new_error);
		return true;
	}
	if (new_entry == new_info.end()) {
		// no new candidates - use current candidates
		return true;
	}
	// both errors have candidates - combine the candidates
	auto current_candidates = StringUtil::Split(current_entry->second, ",");
	auto new_candidates = StringUtil::Split(new_entry->second, ",");
	current_candidates.insert(current_candidates.end(), new_candidates.begin(), new_candidates.end());

	// run the similarity ranking on both sets of candidates
	unordered_set<string> candidates;
	vector<pair<string, double>> scores;
	for (auto &candidate : current_candidates) {
		// split by "." since the candidates might be in the form "table.column"
		auto column_splits = StringUtil::Split(candidate, ".");
		if (column_splits.empty()) {
			continue;
		}
		auto &candidate_column = column_splits.back();
		auto entry = candidates.find(candidate);
		if (entry != candidates.end()) {
			// already found
			continue;
		}
		auto score = StringUtil::SimilarityRating(candidate_column, column_name);
		candidates.insert(candidate);
		scores.emplace_back(std::move(candidate), score);
	}
	// get a new top-n
	auto top_candidates = StringUtil::TopNStrings(scores);
	// get query location (prefer the current error's location, fall back to the new error's)
	auto location = ExtractLocation(current_info);
	if (!location.IsValid()) {
		location = ExtractLocation(new_info);
	}
	QueryErrorContext context(location);
	// generate a new (combined) error
	current = BinderException::ColumnNotFound(Identifier(column_name), StringsToIdentifiers(top_candidates), context);
	return true;
}

void ExpressionBinder::CombineErrors(ErrorData &current, ErrorData new_error) {
	// try to combine missing column exceptions in order to pick the most relevant one
	if (CombineMissingColumns(current, new_error)) {
		// keep the old info
		return;
	}

	// override the error with the new one
	// FIXME: the outermost scope searched always wins here, which can bury a more specific error from
	// the scope the reference was actually written in - e.g. HAVING's "must appear in the GROUP BY
	// clause" is replaced by an enclosing scope's "table not found". Preserving the innermost error
	// unless the outer one is strictly more specific would read better, but it changes many existing
	// error messages, so it is left as is here.
	current = std::move(new_error);
}

ColumnResolution ExpressionBinder::ResolveColumn(ColumnRefExpression &colref, idx_t start) {
	ColumnResolution result;
	for (idx_t depth = start; depth < ScopeCount(); depth++) {
		auto &scope = ScopeAt(depth);
		if (scope.ClaimsAlias(colref)) {
			// the scope has a select-list alias of this name: it owns the reference even though
			// qualification cannot produce a replacement for it
			result.found = true;
			result.depth = depth;
			return result;
		}
		auto qualifier = scope.CreateColumnQualifier();
		ErrorData error;
		auto qualified = qualifier->QualifyColumnName(colref, error);
		if (qualified) {
			result.found = true;
			result.depth = depth;
			result.qualified = std::move(qualified);
			return result;
		}
		if (scope.MatchesGroup(colref)) {
			// the name is one of this scope's groups by alias, which qualification cannot see
			result.found = true;
			result.depth = depth;
			return result;
		}
		if (depth == start) {
			result.error = std::move(error);
		} else {
			CombineErrors(result.error, std::move(error));
		}
	}
	return result;
}

//! Collect the column references that decide which scope owns an expression.
//! A subquery that is only a select list, without FROM clause and aliases, binds no column itself, so its columns count
//! as if they were written outside of it. Other subqueries bind in a chain of their own, and only binding them tells
//! whether a column inside one is theirs, so once there is one, no subquery counts and the columns outside of them
//! decide, as they always did.
//! Lambda parameters are skipped: they are bound by the lambda rather than by any
//! scope, so a parameter that happens to share a name with an outer column must not pull the
//! expression out to that scope. The parameter names are read exactly as `ColumnQualifier` reads them,
//! so the resolver sees the same names a real bind would.
class ResolvableColumnCollector {
public:
	explicit ResolvableColumnCollector(vector<reference<ColumnRefExpression>> &result) : result(result) {
	}

	void CollectExpression(ParsedExpression &expr) {
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::SUBQUERY: {
			auto &subquery = expr.Cast<SubqueryExpression>();
			subquery_depth++;
			// the operand of IN, ANY or ALL belongs to the query of the subquery expression, and counts with it
			if (subquery.GetChild()) {
				CollectExpression(*subquery.GetChildMutable());
			}
			CollectSubquery(*subquery.Subquery()->node);
			subquery_depth--;
			return;
		}
		case ExpressionClass::COLUMN_REF: {
			auto &col_ref = expr.Cast<ColumnRefExpression>();
			// a lambda parameter is always a bare name, so a qualified reference is a column even when its
			// last component happens to match one - `t.x` inside `lambda x: ...` names the table's column
			if (!col_ref.IsQualified() && LambdaExpression::IsLambdaParameter(lambda_params, col_ref.GetName())) {
				return;
			}
			if (subquery_depth == 0) {
				result.push_back(col_ref);
			} else {
				subquery_columns.push_back(col_ref);
			}
			return;
		}
		case ExpressionClass::STAR:
			if (subquery_depth > 0) {
				// a star stands for columns that only binding knows
				all_subqueries_without_from = false;
				return;
			}
			break;
		case ExpressionClass::FUNCTION:
			if (subquery_depth > 0 && expr.Cast<FunctionExpression>().GetQualifiedName().Path().size() > 1) {
				// the qualification can be a column that the call is made on, as in `x.abs()`
				all_subqueries_without_from = false;
				return;
			}
			break;
		case ExpressionClass::LAMBDA: {
			auto &lambda = expr.Cast<LambdaExpression>();
			string error_message;
			auto parameters = lambda.ExtractColumnRefExpressions(error_message);
			if (!error_message.empty()) {
				// the LHS is not a parameter list, so this is the JSON arrow operator: both sides are
				// ordinary expressions and nothing is bound by the lambda
				CollectExpression(*lambda.LeftMutable());
				CollectExpression(*lambda.RightMutable());
				return;
			}
			lambda_params.emplace_back();
			for (auto &parameter : parameters) {
				lambda_params.back().emplace(parameter.get().Cast<ColumnRefExpression>().GetName());
			}
			// only the body can reference anything outside the lambda
			CollectExpression(*lambda.RightMutable());
			lambda_params.pop_back();
			return;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) { CollectExpression(child); });
	}

	//! Add the columns of the subqueries, if every subquery is one without a FROM clause
	void Finish() {
		if (all_subqueries_without_from) {
			result.insert(result.end(), subquery_columns.begin(), subquery_columns.end());
		}
	}

private:
	void CollectSubquery(QueryNode &node) {
		if (node.type != QueryNodeType::SELECT_NODE || !node.cte_map.map.empty()) {
			all_subqueries_without_from = false;
			return;
		}
		auto &select = node.Cast<SelectNode>();
		if (select.from_table && select.from_table->type != TableReferenceType::EMPTY_FROM) {
			all_subqueries_without_from = false;
			return;
		}
		// the other clauses can refer to the select list, also by names it gives its expressions
		if (select.where_clause || !select.groups.group_expressions.empty() || !select.groups.grouping_sets.empty() ||
		    select.having || select.qualify || !node.modifiers.empty()) {
			all_subqueries_without_from = false;
			return;
		}
		for (auto &expr : select.select_list) {
			if (expr->HasAlias()) {
				// later expressions and clauses can refer to the alias
				all_subqueries_without_from = false;
				return;
			}
		}
		for (auto &expr : select.select_list) {
			CollectExpression(*expr);
		}
	}

	vector<reference<ColumnRefExpression>> &result;
	vector<identifier_set_t> lambda_params;
	//! The number of subqueries around the expression that is being collected
	idx_t subquery_depth = 0;
	//! The columns inside subqueries
	vector<reference<ColumnRefExpression>> subquery_columns;
	bool all_subqueries_without_from = true;
};

optional_idx ExpressionBinder::ResolveAggregateOwner(FunctionExpression &aggregate, idx_t start) {
	vector<reference<ColumnRefExpression>> columns;
	ResolvableColumnCollector collector(columns);
	for (auto &child : aggregate.GetArgumentsMutable()) {
		collector.CollectExpression(*child.GetExpressionMutable());
	}
	if (aggregate.Filter()) {
		collector.CollectExpression(*aggregate.FilterMutable());
	}
	if (aggregate.OrderBy()) {
		for (auto &order : aggregate.OrderByMutable()->orders) {
			collector.CollectExpression(*order.expression);
		}
	}
	collector.Finish();
	auto owner = start;
	bool found = false;
	for (auto &colref : columns) {
		auto resolution = ResolveColumn(colref.get(), start);
		if (!resolution.found) {
			// an alias or a column that does not resolve at all - it says nothing about ownership,
			// and the scope we settle on reports the error
			continue;
		}
		if (!found || resolution.depth < owner) {
			owner = resolution.depth;
			found = true;
		}
	}
	return found ? optional_idx(owner) : optional_idx();
}

optional_idx ExpressionBinder::ResolveOuterGroup(vector<reference<ParsedExpression>> &expressions, idx_t start) {
	for (idx_t depth = start; depth < ScopeCount(); depth++) {
		bool all_match = true;
		for (auto &expr : expressions) {
			if (!ScopeAt(depth).MatchesGroup(expr.get())) {
				all_match = false;
				break;
			}
		}
		if (all_match) {
			return depth;
		}
	}
	return optional_idx();
}

} // namespace duckdb
