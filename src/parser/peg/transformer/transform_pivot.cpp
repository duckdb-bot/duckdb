#include "duckdb/parser/peg/ast/unpivot_name_values.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/copy_query_node.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

//! Finds a parameter in the parts of a PIVOT that its values are read from, and in the CTEs that those parts read
class PivotValuesParameterFinder {
public:
	//! adds the visible CTEs of a WITH clause as a scope inside the parent scope
	optional_idx AddScope(CommonTableExpressionMap &ctes, idx_t begin, idx_t end, optional_idx parent) {
		scopes.emplace_back(ctes, begin, end, parent);
		return scopes.size() - 1;
	}

	//! adds a part of the statement to visit, with the innermost scope it is in
	void Push(unique_ptr<ParsedExpression> &expr, optional_idx scope) {
		pending_expressions.emplace_back(*expr, scope);
	}

	void Push(vector<unique_ptr<ParsedExpression>> &expressions, optional_idx scope) {
		for (auto &expr : expressions) {
			Push(expr, scope);
		}
	}

	void Push(UpdateSetInfo &info, optional_idx scope) {
		Push(info.expressions, scope);
		if (info.condition) {
			Push(info.condition, scope);
		}
	}

	void Push(QueryNode &node, optional_idx scope) {
		pending_query_nodes.emplace_back(node, scope);
	}

	void Push(TableRef &ref, optional_idx scope) {
		pending_table_refs.emplace_back(ref, scope);
	}

	//! whether a part that was pushed, or a CTE it reads, has a parameter
	bool HasParameter() {
		bool visited_node = true;
		while (visited_node && !found) {
			visited_node =
			    VisitNext(pending_expressions) || VisitNext(pending_table_refs) || VisitNext(pending_query_nodes);
		}
		return found;
	}

private:
	//! The visible CTEs of a WITH clause, and the scope around them
	struct CTEScope {
		CTEScope(CommonTableExpressionMap &ctes, idx_t begin, idx_t end, optional_idx parent)
		    : ctes(ctes), begin(begin), end(end), parent(parent) {
		}
		reference<CommonTableExpressionMap> ctes;
		//! the visible CTEs, as a CTE only sees those declared before it in its WITH clause
		idx_t begin;
		idx_t end;
		//! the scope around this one, if any
		optional_idx parent;
	};
	//! A part of the statement that is not visited yet, and the innermost scope it is in
	template <class T>
	struct PendingNode {
		PendingNode(T &node, optional_idx scope) : node(node), scope(scope) {
		}
		reference<T> node;
		optional_idx scope;
	};

private:
	//! visits the pending node that was added last, if there is one
	template <class T>
	bool VisitNext(vector<PendingNode<T>> &pending_nodes) {
		if (pending_nodes.empty()) {
			return false;
		}
		auto next = pending_nodes.back();
		pending_nodes.pop_back();
		Visit(next.node.get(), next.scope);
		return true;
	}

	void Visit(ParsedExpression &expr, optional_idx scope) {
		if (expr.GetExpressionClass() == ExpressionClass::PARAMETER) {
			found = true;
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
			Push(*expr.Cast<SubqueryExpression>().SubqueryMutable()->node, scope);
		}
		ParsedExpressionIterator::EnumerateChildren(expr,
		                                            [&](unique_ptr<ParsedExpression> &child) { Push(child, scope); });
	}

	//! the bodies of the CTEs of a node are only visited when they are read
	void Visit(QueryNode &node, optional_idx outer_scope) {
		auto scope = outer_scope;
		if (!node.cte_map.map.empty()) {
			scope = AddScope(node.cte_map, 0, node.cte_map.map.size(), outer_scope);
		}
		switch (node.type) {
		case QueryNodeType::SELECT_NODE: {
			auto &select = node.Cast<SelectNode>();
			Push(select.select_list, scope);
			Push(select.groups.group_expressions, scope);
			for (auto expr : {&select.where_clause, &select.having, &select.qualify}) {
				if (*expr) {
					Push(*expr, scope);
				}
			}
			if (select.from_table) {
				Push(*select.from_table, scope);
			}
			break;
		}
		case QueryNodeType::SET_OPERATION_NODE:
			for (auto &child : node.Cast<SetOperationNode>().children) {
				Push(*child, scope);
			}
			break;
		case QueryNodeType::RECURSIVE_CTE_NODE: {
			auto &recursive = node.Cast<RecursiveCTENode>();
			Push(*recursive.left, scope);
			// the recursive part also sees the CTE itself, which is declared right after the CTEs that its body sees
			auto body_scope = scopes[outer_scope.GetIndex()];
			Push(*recursive.right, AddScope(body_scope.ctes, body_scope.end, body_scope.end + 1, scope));
			break;
		}
		// a DML statement with RETURNING can be read as a CTE, its target table is not read
		case QueryNodeType::INSERT_QUERY_NODE: {
			auto &insert = node.Cast<InsertQueryNode>();
			if (insert.select_statement) {
				Push(*insert.select_statement->node, scope);
			}
			if (insert.on_conflict_info) {
				if (insert.on_conflict_info->set_info) {
					Push(*insert.on_conflict_info->set_info, scope);
				}
				if (insert.on_conflict_info->condition) {
					Push(insert.on_conflict_info->condition, scope);
				}
			}
			Push(insert.returning_list, scope);
			break;
		}
		case QueryNodeType::UPDATE_QUERY_NODE: {
			auto &update = node.Cast<UpdateQueryNode>();
			if (update.from_table) {
				Push(*update.from_table, scope);
			}
			if (update.set_info) {
				Push(*update.set_info, scope);
			}
			Push(update.returning_list, scope);
			break;
		}
		case QueryNodeType::DELETE_QUERY_NODE: {
			auto &del = node.Cast<DeleteQueryNode>();
			for (auto &ref : del.using_clauses) {
				Push(*ref, scope);
			}
			if (del.condition) {
				Push(del.condition, scope);
			}
			Push(del.returning_list, scope);
			break;
		}
		case QueryNodeType::COPY_QUERY_NODE: {
			auto &info = *node.Cast<CopyQueryNode>().info;
			if (info.select_statement) {
				Push(*info.select_statement, scope);
			} else if (!info.is_from) {
				ReadCTE(info.GetQualifiedName(), scope);
			}
			if (info.file_path_expression) {
				Push(info.file_path_expression, scope);
			}
			for (auto &option : info.parsed_options) {
				if (option.second) {
					Push(option.second, scope);
				}
			}
			break;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(
		    node, [&](unique_ptr<ParsedExpression> &child) { Push(child, scope); });
	}

	void Visit(TableRef &ref, optional_idx scope) {
		switch (ref.type) {
		case TableReferenceType::BASE_TABLE: {
			auto &table = ref.Cast<BaseTableRef>();
			// the AT clause of a table is read, that of a CTE is not
			if (!ReadCTE(table.GetQualifiedName(), scope) && table.at_clause) {
				Push(table.at_clause->ExpressionMutable(), scope);
			}
			break;
		}
		case TableReferenceType::JOIN: {
			auto &join = ref.Cast<JoinRef>();
			Push(*join.left, scope);
			Push(*join.right, scope);
			for (auto expr : {&join.condition, &join.ranking_expression}) {
				if (*expr) {
					Push(*expr, scope);
				}
			}
			break;
		}
		case TableReferenceType::SUBQUERY:
			Push(*ref.Cast<SubqueryRef>().subquery->node, scope);
			break;
		case TableReferenceType::PIVOT: {
			auto &pivot = ref.Cast<PivotRef>();
			Push(*pivot.source, scope);
			Push(pivot.aggregates, scope);
			for (auto &column : pivot.pivots) {
				Push(column.pivot_expressions, scope);
				for (auto &entry : column.entries) {
					if (entry.expr) {
						Push(entry.expr, scope);
					}
				}
			}
			break;
		}
		case TableReferenceType::SHOW_REF: {
			auto &show = ref.Cast<ShowRef>();
			if (show.query) {
				Push(*show.query, scope);
			}
			break;
		}
		default:
			// the other references hold expressions, and MATCH_RECOGNIZE also an input table
			ParsedExpressionIterator::EnumerateTableRefChildren(
			    ref, [&](unique_ptr<ParsedExpression> &child) { Push(child, scope); },
			    [&](TableRef &child) {
				    if (&child != &ref) {
					    Push(child, scope);
				    }
			    });
			break;
		}
	}

	//! whether a table name reads a CTE, which is then visited once, with the CTEs in scope where it is defined
	bool ReadCTE(const QualifiedName &name, optional_idx scope) {
		if (!name.Catalog().empty() || !name.Schema().empty()) {
			return false;
		}
		while (scope.IsValid()) {
			auto defining_scope = scopes[scope.GetIndex()];
			scope = defining_scope.parent;
			auto &ctes = defining_scope.ctes.get();
			auto entry = ctes.map.find(name.Name());
			if (entry == ctes.map.end()) {
				continue;
			}
			auto index = NumericCast<idx_t>(entry - ctes.map.begin());
			if (index < defining_scope.begin || index >= defining_scope.end) {
				continue;
			}
			auto &cte = *entry->second;
			if (visited_ctes.insert(cte).second && cte.query_node) {
				Push(*cte.query_node, AddScope(ctes, 0, index, defining_scope.parent));
			}
			return true;
		}
		return false;
	}

private:
	//! the scopes of all parts of the statement, each of which names the scope around it
	vector<CTEScope> scopes;
	//! what is left to visit, kept here as recursion would overflow the stack on a long chain of CTEs
	vector<PendingNode<ParsedExpression>> pending_expressions;
	vector<PendingNode<TableRef>> pending_table_refs;
	vector<PendingNode<QueryNode>> pending_query_nodes;
	reference_set_t<CommonTableExpressionInfo> visited_ctes;
	bool found = false;
};

//! Whether a parameter that is not counted with the source reaches the query of the values of a pivot column
static bool PivotValuesHaveParameter(PEGTransformer &transformer, TableRef &source, vector<PivotColumn> &columns) {
	PivotValuesParameterFinder finder;
	optional_idx scope;
	for (auto &ctes : transformer.stored_cte_map) {
		scope = finder.AddScope(ctes.get(), 0, ctes.get().map.size(), scope);
	}
	bool reads_source = false;
	for (auto &column : columns) {
		if (!column.pivot_enum.empty() || !column.entries.empty()) {
			continue;
		}
		if (column.subquery) {
			// the values are read from the subquery, which does not see the CTEs around the PIVOT
			finder.Push(*column.subquery, optional_idx());
		} else {
			// the values are those of the pivot expression over the source
			finder.Push(column.pivot_expressions, scope);
			reads_source = true;
		}
	}
	if (reads_source) {
		finder.Push(source, scope);
	}
	return finder.HasParameter();
}

void PEGTransformerFactory::AddPivotEntry(PEGTransformer &transformer, string enum_name, unique_ptr<SelectNode> base,
                                          unique_ptr<ParsedExpression> column, unique_ptr<QueryNode> subquery,
                                          bool has_parameters) {
	auto result = make_uniq<CreatePivotEntry>();
	result->enum_name = std::move(enum_name);
	result->base = std::move(base);
	result->column = std::move(column);
	result->subquery = std::move(subquery);
	result->has_parameters = has_parameters;
	transformer.pivot_entries.push_back(std::move(result));
}

vector<PivotColumn> PEGTransformerFactory::TransformPivotOn(PEGTransformer &transformer,
                                                            vector<PivotColumn> pivot_column_list) {
	return pivot_column_list;
}

vector<PivotColumn> PEGTransformerFactory::TransformPivotColumnList(PEGTransformer &transformer,
                                                                    vector<PivotColumn> pivot_column_entry) {
	for (auto &col : pivot_column_entry) {
		// Re-wrap multiple pivot expressions into a single row() only when IN entries are
		// scalar (size 1). For tuple IN entries the unpacked expressions must stay separate
		// so that each value maps to its corresponding pivot expression.
		if (col.pivot_expressions.size() > 1) {
			bool has_tuple_entries = false;
			for (auto &entry : col.entries) {
				if (entry.values.size() > 1) {
					has_tuple_entries = true;
					break;
				}
			}
			if (!has_tuple_entries) {
				auto row_function = make_uniq<FunctionExpression>("row", std::move(col.pivot_expressions));
				col.pivot_expressions.push_back(std::move(row_function));
			}
		}
		for (auto &expr : col.pivot_expressions) {
			if (expr->IsScalar()) {
				throw ParserException(expr->GetQueryLocation(), "Cannot pivot on constant value \"%s\"",
				                      expr->ToString());
			}
			if (expr->HasSubquery()) {
				throw ParserException(expr->GetQueryLocation(), "Cannot pivot on subquery \"%s\"", expr->ToString());
			}
		}
	}
	return pivot_column_entry;
}

PivotColumn PEGTransformerFactory::TransformPivotColumnSubquery(PEGTransformer &transformer,
                                                                unique_ptr<ParsedExpression> base_expression,
                                                                unique_ptr<SelectStatement> select_statement_internal) {
	PivotColumn result;
	result.pivot_expressions.push_back(std::move(base_expression));
	result.subquery = std::move(select_statement_internal->node);
	return result;
}

PivotColumn PEGTransformerFactory::TransformPivotColumnExpression(PEGTransformer &transformer,
                                                                  unique_ptr<ParsedExpression> expression) {
	PivotColumn result;
	result.pivot_expressions.push_back(std::move(expression));
	return result;
}

void PEGTransformerFactory::InitializePivotStatementTrampoline(PEGTransformer &transformer,
                                                               GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	process.manual_state = transformer.ParamCount();
	process.ReserveChildSlots(5);
	process.PushChild({list_pr.GetChild(1)}, 0);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizePivotStatementTrampoline(PEGTransformer &transformer,
                                                        GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto &pivot_columns = list_pr.Child<OptionalParseResult>(2);
	auto &pivot_aggregates = list_pr.Child<OptionalParseResult>(3);
	auto &pivot_group = list_pr.Child<OptionalParseResult>(4);
	if (!pivot_columns.HasResult() && !pivot_aggregates.HasResult() && !pivot_group.HasResult()) {
		throw ParserException("PIVOT requires an ON, USING or GROUP BY clause");
	}
	if (!process.child_results[4]) {
		bool has_parameters = transformer.ParamCount() > process.manual_state;
		process.SetChildResult(4, transformer.MakeResult<bool>(has_parameters));
		bool pushed_child = false;
		if (pivot_group.HasResult()) {
			process.PushChild({pivot_group.GetResult()}, 3);
			pushed_child = true;
		}
		if (pivot_aggregates.HasResult()) {
			process.PushChild({pivot_aggregates.GetResult()}, 2);
			pushed_child = true;
		}
		if (pivot_columns.HasResult()) {
			process.PushChild({pivot_columns.GetResult()}, 1);
			pushed_child = true;
		}
		if (pushed_child) {
			return nullptr;
		}
	}
	auto source = process.TakeResult<unique_ptr<TableRef>>(0);
	auto has_parameters = process.TakeResult<bool>(4);
	auto select_node = make_uniq<SelectNode>();
	if (!pivot_columns.HasResult()) {
		select_node->from_table = std::move(source);
		if (pivot_group.HasResult()) {
			auto pivot_group_list = process.TakeResult<vector<string>>(3);
			GroupingSet set;
			for (idx_t gr = 0; gr < pivot_group_list.size(); gr++) {
				auto &group = pivot_group_list[gr];
				auto colref = make_uniq<ColumnRefExpression>(Identifier(group));
				select_node->select_list.push_back(colref->Copy());
				select_node->groups.group_expressions.push_back(std::move(colref));
				set.insert(ProjectionIndex(gr));
			}
			select_node->groups.grouping_sets.push_back(std::move(set));
		}
		auto &pivot_using_opt = list_pr.Child<OptionalParseResult>(3);
		if (pivot_using_opt.HasResult()) {
			auto pivot_using = process.TakeResult<vector<unique_ptr<ParsedExpression>>>(2);
			for (auto &col : pivot_using) {
				select_node->select_list.push_back(std::move(col));
			}
		}
		auto result = make_uniq<SelectStatement>();
		result->node = std::move(select_node);
		return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(result));
	}
	auto columns = process.TakeResult<vector<PivotColumn>>(1);
	// a parameter in a CTE, a pivot expression or an IN subquery is not counted while the source is transformed
	has_parameters = has_parameters || PivotValuesHaveParameter(transformer, *source, columns);
	for (idx_t c = 0; c < columns.size(); c++) {
		auto &col = columns[c];
		if (!col.pivot_enum.empty() || !col.entries.empty()) {
			continue;
		}
		if (col.pivot_expressions.size() != 1) {
			throw InternalException("PIVOT statement with multiple names in pivot entry!?");
		}
		auto enum_name = "__pivot_enum_" + UUID::ToString(UUID::GenerateRandomUUID());

		auto new_select = make_uniq<SelectNode>();
		transformer.ExtractCTEsRecursive(new_select->cte_map);
		new_select->from_table = source->Copy();
		AddPivotEntry(transformer, enum_name, std::move(new_select), col.pivot_expressions[0]->Copy(),
		              std::move(col.subquery), has_parameters);
		col.pivot_enum = Identifier(enum_name);
	}

	select_node->select_list.push_back(make_uniq<StarExpression>());
	auto pivot_ref = make_uniq<PivotRef>();
	pivot_ref->source = std::move(source);
	if (pivot_aggregates.HasResult()) {
		pivot_ref->aggregates = process.TakeResult<vector<unique_ptr<ParsedExpression>>>(2);
	} else {
		vector<unique_ptr<ParsedExpression>> children;
		auto function = make_uniq<FunctionExpression>("count_star", std::move(children));
		pivot_ref->aggregates.push_back(std::move(function));
	}
	if (pivot_group.HasResult()) {
		pivot_ref->groups = StringsToIdentifiers(process.TakeResult<vector<string>>(3));
	}
	pivot_ref->pivots = std::move(columns);
	select_node->from_table = std::move(pivot_ref);
	auto result = make_uniq<SelectStatement>();
	result->node = std::move(select_node);
	return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(result));
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformPivotUsing(PEGTransformer &transformer,
                                           vector<unique_ptr<ParsedExpression>> target_list) {
	return target_list;
}

vector<string> PEGTransformerFactory::TransformUnpivotHeaderSingle(PEGTransformer &transformer,
                                                                   const Identifier &col_id_or_string) {
	vector<string> result;
	result.push_back(col_id_or_string.GetIdentifierName());
	return result;
}

vector<string> PEGTransformerFactory::TransformUnpivotHeaderList(PEGTransformer &transformer,
                                                                 const vector<Identifier> &col_id_or_string) {
	return IdentifiersToStrings(col_id_or_string);
}

vector<string> PEGTransformerFactory::TransformParenthesizedNameList(PEGTransformer &transformer,
                                                                     const vector<Identifier> &col_id_or_string) {
	return IdentifiersToStrings(col_id_or_string);
}

vector<string> PEGTransformerFactory::TransformBareNameList(PEGTransformer &transformer,
                                                            const vector<Identifier> &col_id_or_string) {
	return IdentifiersToStrings(col_id_or_string);
}

bool PEGTransformerFactory::TransformIncludeNulls(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformExcludeNulls(PEGTransformer &transformer) {
	return false;
}

void PEGTransformerFactory::InitializeUnpivotStatementTrampoline(PEGTransformer &transformer,
                                                                 GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	process.manual_state = transformer.ParamCount();
	process.ReserveChildSlots(4);
	process.PushChild({list_pr.GetChild(1)}, 0);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizeUnpivotStatementTrampoline(PEGTransformer &transformer,
                                                          GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	if (!process.child_results[3]) {
		bool has_parameters = transformer.ParamCount() > process.manual_state;
		process.SetChildResult(3, transformer.MakeResult<bool>(has_parameters));
		auto &unpivot_names_opt = list_pr.Child<OptionalParseResult>(4);
		if (unpivot_names_opt.HasResult()) {
			process.PushChild({unpivot_names_opt.GetResult()}, 2);
		}
		process.PushChild({list_pr.GetChild(3)}, 1);
		return nullptr;
	}
	auto source = process.TakeResult<unique_ptr<TableRef>>(0);
	auto has_parameters = process.TakeResult<bool>(3);
	auto target_list = process.TakeResult<vector<unique_ptr<ParsedExpression>>>(1);
	auto &unpivot_names_opt = list_pr.Child<OptionalParseResult>(4);
	vector<PivotColumn> columns;
	UnpivotNameValues name_and_values;
	if (!unpivot_names_opt.HasResult()) {
		PivotColumn col;
		col.unpivot_names.push_back("name");
		for (auto &expr : target_list) {
			PivotColumnEntry entry;
			entry.alias = expr->GetAlias();
			entry.expr = std::move(expr);
			col.entries.push_back(std::move(entry));
		}
		columns.push_back(std::move(col));
	} else {
		name_and_values = process.TakeResult<UnpivotNameValues>(2);
		auto unpivot_list_size = name_and_values.column.unpivot_names.size();
		if (unpivot_list_size != 1) {
			throw NotImplementedException("Only expected a single value after NAME, got %d instead.",
			                              unpivot_list_size);
		}
		auto &col = name_and_values.column;
		for (auto &expr : target_list) {
			PivotColumnEntry entry;
			entry.alias = expr->GetAlias();
			entry.expr = std::move(expr);
			col.entries.push_back(std::move(entry));
		}
		columns.push_back(std::move(col));
	}

	for (idx_t c = 0; c < columns.size(); c++) {
		auto &col = columns[c];
		if (!col.pivot_enum.empty() || !col.entries.empty()) {
			continue;
		}
		if (col.pivot_expressions.size() != 1) {
			throw InternalException("PIVOT statement with multiple names in pivot entry!?");
		}
		auto enum_name = "__pivot_enum_" + UUID::ToString(UUID::GenerateRandomUUID());

		auto new_select = make_uniq<SelectNode>();
		transformer.ExtractCTEsRecursive(new_select->cte_map);
		new_select->from_table = source->Copy();
		AddPivotEntry(transformer, enum_name, std::move(new_select), col.pivot_expressions[0]->Copy(),
		              std::move(col.subquery), has_parameters);
		col.pivot_enum = Identifier(enum_name);
	}

	auto pivot_ref = make_uniq<PivotRef>();
	pivot_ref->source = std::move(source);
	if (!unpivot_names_opt.HasResult()) {
		pivot_ref->unpivot_names.push_back("value");
	} else {
		pivot_ref->unpivot_names = name_and_values.unpivot_names;
	}

	pivot_ref->pivots = std::move(columns);
	auto select_node = make_uniq<SelectNode>();
	select_node->from_table = std::move(pivot_ref);
	select_node->select_list.push_back(make_uniq<StarExpression>());
	auto result = make_uniq<SelectStatement>();
	result->node = std::move(select_node);
	return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(result));
}

UnpivotNameValues PEGTransformerFactory::TransformIntoNameValues(PEGTransformer &transformer,
                                                                 const Identifier &col_id_or_string,
                                                                 const vector<string> &optional_parens_name_list) {
	UnpivotNameValues result;
	PivotColumn column;
	column.unpivot_names.push_back(Identifier(col_id_or_string));
	result.column = std::move(column);
	result.unpivot_names = StringsToIdentifiers(optional_parens_name_list);
	return result;
}
} // namespace duckdb
