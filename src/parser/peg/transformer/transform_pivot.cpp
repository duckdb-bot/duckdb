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
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/query_node/copy_query_node.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

//! Finds a parameter in a PIVOT source, also in the CTEs it reads, which the query of the pivot values copies
struct PivotSourceParameterFinder {
	struct CTEScope {
		CTEScope(CommonTableExpressionMap &ctes, idx_t begin, idx_t end) : ctes(ctes), begin(begin), end(end) {
		}
		reference<CommonTableExpressionMap> ctes;
		//! the visible CTEs, as a CTE only sees those declared before it in its WITH clause
		idx_t begin;
		idx_t end;
	};
	//! the CTEs in scope, the innermost last
	vector<CTEScope> scopes;
	reference_set_t<CommonTableExpressionInfo> visited_ctes;
	bool found = false;

	void Visit(unique_ptr<ParsedExpression> &expr) {
		if (expr->GetExpressionClass() == ExpressionClass::PARAMETER) {
			found = true;
			return;
		}
		if (expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
			Visit(*expr->Cast<SubqueryExpression>().SubqueryMutable()->node);
		}
		ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) { Visit(child); });
	}

	void Visit(vector<unique_ptr<ParsedExpression>> &expressions) {
		for (auto &expr : expressions) {
			Visit(expr);
		}
	}

	void Visit(UpdateSetInfo &info) {
		Visit(info.expressions);
		if (info.condition) {
			Visit(info.condition);
		}
	}

	//! the bodies of the CTEs of a node are only visited when they are read
	void Visit(QueryNode &node) {
		scopes.emplace_back(node.cte_map, 0, node.cte_map.map.size());
		switch (node.type) {
		case QueryNodeType::SELECT_NODE: {
			auto &select = node.Cast<SelectNode>();
			Visit(select.select_list);
			Visit(select.groups.group_expressions);
			for (auto expr : {&select.where_clause, &select.having, &select.qualify}) {
				if (*expr) {
					Visit(*expr);
				}
			}
			if (select.from_table) {
				Visit(*select.from_table);
			}
			break;
		}
		case QueryNodeType::SET_OPERATION_NODE:
			for (auto &child : node.Cast<SetOperationNode>().children) {
				Visit(*child);
			}
			break;
		case QueryNodeType::RECURSIVE_CTE_NODE: {
			auto &recursive = node.Cast<RecursiveCTENode>();
			Visit(*recursive.left);
			// the recursive part also sees the CTE itself, which ends the scope it is defined in, but the CTEs it
			// reads do not
			D_ASSERT(scopes.size() >= 2);
			auto defining_scope = scopes[scopes.size() - 2];
			scopes.emplace_back(defining_scope.ctes, defining_scope.end, defining_scope.end + 1);
			Visit(*recursive.right);
			scopes.pop_back();
			break;
		}
		// a DML statement with RETURNING can be read as a CTE, its target table is not read
		case QueryNodeType::INSERT_QUERY_NODE: {
			auto &insert = node.Cast<InsertQueryNode>();
			if (insert.select_statement) {
				Visit(*insert.select_statement->node);
			}
			if (insert.on_conflict_info) {
				if (insert.on_conflict_info->set_info) {
					Visit(*insert.on_conflict_info->set_info);
				}
				if (insert.on_conflict_info->condition) {
					Visit(insert.on_conflict_info->condition);
				}
			}
			Visit(insert.returning_list);
			break;
		}
		case QueryNodeType::UPDATE_QUERY_NODE: {
			auto &update = node.Cast<UpdateQueryNode>();
			if (update.from_table) {
				Visit(*update.from_table);
			}
			if (update.set_info) {
				Visit(*update.set_info);
			}
			Visit(update.returning_list);
			break;
		}
		case QueryNodeType::DELETE_QUERY_NODE: {
			auto &del = node.Cast<DeleteQueryNode>();
			for (auto &ref : del.using_clauses) {
				Visit(*ref);
			}
			if (del.condition) {
				Visit(del.condition);
			}
			Visit(del.returning_list);
			break;
		}
		case QueryNodeType::MERGE_QUERY_NODE: {
			auto &merge = node.Cast<MergeQueryNode>();
			if (merge.source) {
				Visit(*merge.source);
			}
			if (merge.join_condition) {
				Visit(merge.join_condition);
			}
			for (auto &entry : merge.actions) {
				for (auto &action : entry.second) {
					if (action->condition) {
						Visit(action->condition);
					}
					if (action->update_info) {
						Visit(*action->update_info);
					}
					Visit(action->expressions);
				}
			}
			Visit(merge.returning_list);
			break;
		}
		case QueryNodeType::COPY_QUERY_NODE: {
			auto &info = *node.Cast<CopyQueryNode>().info;
			if (info.select_statement) {
				Visit(*info.select_statement);
			} else if (!info.is_from) {
				VisitCTE(info.GetQualifiedName());
			}
			if (info.file_path_expression) {
				Visit(info.file_path_expression);
			}
			for (auto &option : info.parsed_options) {
				if (option.second) {
					Visit(option.second);
				}
			}
			break;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(
		    node, [&](unique_ptr<ParsedExpression> &child) { Visit(child); });
		scopes.pop_back();
	}

	void Visit(TableRef &ref) {
		switch (ref.type) {
		case TableReferenceType::BASE_TABLE:
			VisitCTE(ref.Cast<BaseTableRef>().GetQualifiedName());
			break;
		case TableReferenceType::JOIN: {
			auto &join = ref.Cast<JoinRef>();
			Visit(*join.left);
			Visit(*join.right);
			if (join.condition) {
				Visit(join.condition);
			}
			break;
		}
		case TableReferenceType::SUBQUERY:
			Visit(*ref.Cast<SubqueryRef>().subquery->node);
			break;
		case TableReferenceType::PIVOT: {
			auto &pivot = ref.Cast<PivotRef>();
			Visit(*pivot.source);
			Visit(pivot.aggregates);
			for (auto &column : pivot.pivots) {
				Visit(column.pivot_expressions);
				for (auto &entry : column.entries) {
					if (entry.expr) {
						Visit(entry.expr);
					}
				}
			}
			break;
		}
		case TableReferenceType::SHOW_REF: {
			auto &show = ref.Cast<ShowRef>();
			if (show.query) {
				Visit(*show.query);
			}
			break;
		}
		default:
			// the other references hold expressions, and MATCH_RECOGNIZE also an input table
			ParsedExpressionIterator::EnumerateTableRefChildren(
			    ref, [&](unique_ptr<ParsedExpression> &child) { Visit(child); },
			    [&](TableRef &child) {
				    if (&child != &ref) {
					    Visit(child);
				    }
			    });
			break;
		}
	}

	//! visits the CTE a table name reads, with the CTEs in scope where it is defined
	void VisitCTE(const QualifiedName &name) {
		if (!name.Catalog().empty() || !name.Schema().empty()) {
			return;
		}
		for (idx_t i = scopes.size(); i > 0; i--) {
			auto &scope = scopes[i - 1];
			auto &ctes = scope.ctes.get();
			auto entry = ctes.map.find(name.Name());
			if (entry == ctes.map.end()) {
				continue;
			}
			auto index = NumericCast<idx_t>(entry - ctes.map.begin());
			if (index < scope.begin || index >= scope.end) {
				continue;
			}
			auto &cte = *entry->second;
			if (!visited_ctes.insert(cte).second || !cte.query_node) {
				return;
			}
			auto outer_scopes = scopes;
			scopes.erase(scopes.begin() + NumericCast<int64_t>(i - 1), scopes.end());
			scopes.emplace_back(ctes, 0, index);
			Visit(*cte.query_node);
			scopes = std::move(outer_scopes);
			return;
		}
	}
};

static bool SourceHasParameter(PEGTransformer &transformer, TableRef &source) {
	PivotSourceParameterFinder finder;
	for (auto &ctes : transformer.stored_cte_map) {
		finder.scopes.emplace_back(ctes.get(), 0, ctes.get().map.size());
	}
	finder.Visit(source);
	return finder.found;
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
	// a parameter in a CTE that the source reads is not counted while the source is transformed
	auto has_parameters = process.TakeResult<bool>(4) || SourceHasParameter(transformer, *source);
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
