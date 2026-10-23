#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

namespace {

//! Qualifies the names in the body of a macro that the macro's search path, which starts with its own schema as for a
//! view, resolves to another entry than the caller's search path. The arguments are substituted afterwards, so their
//! names still resolve where the macro is called.
class MacroBodyQualifier {
public:
	MacroBodyQualifier(Binder &binder, const CatalogEntryRetriever &binder_retriever,
	                   vector<CatalogSearchEntry> macro_search_path)
	    : binder(binder), caller_retriever(binder.context), macro_retriever(binder.context) {
		// looking names up must not register dependencies, the real bind does that for the names it uses
		caller_retriever.Inherit(binder_retriever);
		caller_retriever.SetCallback(nullptr);
		macro_retriever.Inherit(binder_retriever);
		macro_retriever.SetCallback(nullptr);
		macro_retriever.SetSearchPath(std::move(macro_search_path));
	}

	void QualifyExpression(ParsedExpression &expr) {
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::FUNCTION: {
			auto &function = expr.Cast<FunctionExpression>();
			auto qualified = Qualify(CatalogType::SCALAR_FUNCTION_ENTRY, function.GetQualifiedName());
			if (qualified) {
				function.SetQualifiedName(std::move(*qualified));
			}
			break;
		}
		case ExpressionClass::WINDOW: {
			auto &window = expr.Cast<WindowExpression>();
			auto qualified = Qualify(CatalogType::AGGREGATE_FUNCTION_ENTRY, window.GetQualifiedName());
			if (qualified) {
				window.SetQualifiedName(std::move(*qualified));
			}
			break;
		}
		case ExpressionClass::CAST:
			QualifyExpression(expr.Cast<CastExpression>().TargetTypeMutable());
			break;
		case ExpressionClass::TYPE: {
			auto &type = expr.Cast<TypeExpression>();
			auto qualified = Qualify(CatalogType::TYPE_ENTRY, type.GetQualifiedName());
			if (qualified) {
				type.SetQualifiedName(std::move(*qualified));
			}
			break;
		}
		case ExpressionClass::SUBQUERY:
			QualifyNode(*expr.Cast<SubqueryExpression>().Subquery()->node);
			break;
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) { QualifyExpression(child); });
	}

	void QualifyNode(QueryNode &node) {
		identifier_set_t names;
		for (auto &entry : node.cte_map.map) {
			names.insert(entry.first);
		}
		cte_names.push_back(std::move(names));
		for (auto &entry : node.cte_map.map) {
			if (entry.second->query_node) {
				QualifyNode(*entry.second->query_node);
			}
		}
		switch (node.type) {
		case QueryNodeType::SELECT_NODE: {
			auto &select = node.Cast<SelectNode>();
			if (select.from_table) {
				QualifyTableRef(*select.from_table);
			}
			for (auto &expr : select.select_list) {
				// a qualified name changes the name of an unnamed column, which keeps the one it had
				auto name = expr->HasAlias() ? Identifier() : expr->GetName();
				const auto qualified_before = qualified_count;
				QualifyExpression(*expr);
				if (qualified_count != qualified_before && !name.empty()) {
					expr->SetAlias(std::move(name));
				}
			}
			if (select.where_clause) {
				QualifyExpression(*select.where_clause);
			}
			for (auto &group : select.groups.group_expressions) {
				QualifyExpression(*group);
			}
			if (select.having) {
				QualifyExpression(*select.having);
			}
			if (select.qualify) {
				QualifyExpression(*select.qualify);
			}
			break;
		}
		case QueryNodeType::SET_OPERATION_NODE:
			for (auto &child : node.Cast<SetOperationNode>().children) {
				QualifyNode(*child);
			}
			break;
		case QueryNodeType::RECURSIVE_CTE_NODE: {
			auto &recursive = node.Cast<RecursiveCTENode>();
			QualifyNode(*recursive.left);
			QualifyNode(*recursive.right);
			break;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(
		    node, [&](unique_ptr<ParsedExpression> &child) { QualifyExpression(*child); });
		cte_names.pop_back();
	}

private:
	void QualifyTableRef(TableRef &ref) {
		switch (ref.type) {
		case TableReferenceType::BASE_TABLE: {
			auto &table = ref.Cast<BaseTableRef>();
			if (IsVisibleCTE(table.Table())) {
				break;
			}
			auto qualified = Qualify(CatalogType::TABLE_ENTRY, table.GetQualifiedName());
			if (qualified) {
				table.GetQualifiedNameMutable() = std::move(*qualified);
			}
			break;
		}
		case TableReferenceType::TABLE_FUNCTION: {
			auto &function = ref.Cast<TableFunctionRef>().function;
			if (function->GetExpressionClass() == ExpressionClass::FUNCTION) {
				auto &function_expr = function->Cast<FunctionExpression>();
				auto qualified = Qualify(CatalogType::TABLE_FUNCTION_ENTRY, function_expr.GetQualifiedName());
				if (qualified) {
					function_expr.SetQualifiedName(std::move(*qualified));
				}
				// the arguments are expressions of the body as well
				for (auto &argument : function_expr.GetArgumentsMutable()) {
					QualifyExpression(*argument.GetExpressionMutable());
				}
			}
			break;
		}
		case TableReferenceType::SUBQUERY:
			QualifyNode(*ref.Cast<SubqueryRef>().subquery->node);
			break;
		case TableReferenceType::JOIN: {
			auto &join = ref.Cast<JoinRef>();
			QualifyTableRef(*join.left);
			QualifyTableRef(*join.right);
			if (join.condition) {
				QualifyExpression(*join.condition);
			}
			break;
		}
		default:
			// the iterator also hands over the reference itself, which is not walked again
			ParsedExpressionIterator::EnumerateTableRefChildren(
			    ref, [&](unique_ptr<ParsedExpression> &child) { QualifyExpression(*child); },
			    [&](TableRef &child) {
				    if (&child != &ref) {
					    QualifyTableRef(child);
				    }
			    });
			break;
		}
	}

	bool IsVisibleCTE(const Identifier &name) {
		for (auto &names : cte_names) {
			if (names.count(name)) {
				return true;
			}
		}
		// the query that calls the macro can have a CTE of that name
		return binder.GetCTEBinding(BindingAlias(name)) != nullptr;
	}

	//! The full name of the entry the macro's search path finds, if the caller's search path finds another one
	unique_ptr<QualifiedName> Qualify(CatalogType type, const QualifiedName &name) {
		if (name.Path().size() != 1) {
			return nullptr;
		}
		auto macro_entry = LookUp(macro_retriever, type, name);
		if (!macro_entry) {
			return nullptr;
		}
		auto caller_entry = LookUp(caller_retriever, type, name);
		if (caller_entry.get() == macro_entry.get()) {
			return nullptr;
		}
		auto &schema = macro_entry->ParentSchema();
		qualified_count++;
		return make_uniq<QualifiedName>(
		    QualifiedName::FromCatalogSchema(schema.ParentCatalog().GetName(), schema.GetSchemaPath(), name.Name()));
	}

	static optional_ptr<CatalogEntry> LookUp(CatalogEntryRetriever &retriever, CatalogType type,
	                                         const QualifiedName &name) {
		try {
			return retriever.GetEntry(EntryLookupInfo(type, name), OnEntryNotFound::RETURN_NULL);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() == ExceptionType::INTERRUPT || error.Type() == ExceptionType::FATAL ||
			    error.Type() == ExceptionType::INTERNAL) {
				throw;
			}
			// the real bind reports why the name does not resolve
			return nullptr;
		}
	}

private:
	Binder &binder;
	CatalogEntryRetriever caller_retriever;
	CatalogEntryRetriever macro_retriever;
	//! The number of names qualified so far
	idx_t qualified_count = 0;
	//! The CTEs of the body that are visible where the walk is, innermost last
	vector<identifier_set_t> cte_names;
};

} // namespace

void Binder::QualifyMacroBody(StandardEntry &macro, ParsedExpression &body) {
	MacroBodyQualifier qualifier(*this, entry_retriever, GetMacroSearchPath(macro));
	qualifier.QualifyExpression(body);
}

void Binder::QualifyMacroBody(StandardEntry &macro, QueryNode &body) {
	MacroBodyQualifier qualifier(*this, entry_retriever, GetMacroSearchPath(macro));
	qualifier.QualifyNode(body);
}

vector<CatalogSearchEntry> Binder::GetMacroSearchPath(StandardEntry &macro) {
	return GetSearchPath(macro.ParentCatalog(), macro.ParentSchema().name, true);
}

unique_ptr<QueryNode> Binder::BindTableMacro(FunctionExpression &function, TableMacroCatalogEntry &macro_func,
                                             idx_t depth) {
	// expanding nested macros can take a long time, allow the query to be interrupted
	context.InterruptCheck();
	// validate the arguments and separate positional and default arguments
	vector<unique_ptr<ParsedExpression>> positional_arguments;
	InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> named_arguments;

	auto bind_result = MacroFunction::BindMacroFunction(*this, macro_func.macros, macro_func.name, function,
	                                                    positional_arguments, named_arguments, depth);
	if (!bind_result.error.empty()) {
		throw BinderException(function, bind_result.error);
	}
	auto &macro_def = *macro_func.macros[bind_result.function_idx.GetIndex()];

	auto new_macro_binding =
	    MacroFunction::CreateDummyBinding(macro_def, macro_func.name, positional_arguments, named_arguments);
	new_macro_binding->arguments = &positional_arguments;

	// We need an ExpressionBinder so that we can call ExpressionBinder::ReplaceMacroParametersRecursive()
	auto eb = ExpressionBinder(*this, this->context);

	eb.macro_binding = new_macro_binding.get();
	vector<identifier_set_t> lambda_params;

	auto node = macro_def.Cast<TableMacroFunction>().query_node->Copy();
	QualifyMacroBody(macro_func, *node);
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    *node, [&](unique_ptr<ParsedExpression> &child) { eb.ReplaceMacroParameters(child, lambda_params); });

	return node;
}

} // namespace duckdb
