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
#include "duckdb/parser/tableref/match_recognize_ref.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

namespace {

//! Qualifies the names in the body of a macro that the macro's search path, which starts with its own schema as for a
//! view, resolves to another entry than the caller's search path. The new names are set once the arguments are
//! substituted, so the names of the arguments still resolve where the macro is called and a column keeps the name it
//! has without them.
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
		case ExpressionClass::FUNCTION:
			Qualify(CatalogType::SCALAR_FUNCTION_ENTRY, expr.Cast<FunctionExpression>().GetQualifiedNameMutable());
			break;
		case ExpressionClass::WINDOW:
			Qualify(CatalogType::AGGREGATE_FUNCTION_ENTRY, expr.Cast<WindowExpression>().GetQualifiedNameMutable());
			break;
		case ExpressionClass::CAST:
			QualifyExpression(expr.Cast<CastExpression>().TargetTypeMutable());
			break;
		case ExpressionClass::TYPE:
			Qualify(CatalogType::TYPE_ENTRY, expr.Cast<TypeExpression>().GetQualifiedNameMutable());
			break;
		case ExpressionClass::SUBQUERY:
			QualifyNode(*expr.Cast<SubqueryExpression>().Subquery()->node);
			break;
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) { QualifyExpression(child); });
	}

	void QualifyNode(QueryNode &node) {
		// a CTE sees the CTEs before it
		cte_names.emplace_back();
		for (auto &entry : node.cte_map.map) {
			if (entry.second->query_node) {
				QualifyNode(*entry.second->query_node);
			}
			cte_names.back().insert(entry.first);
		}
		switch (node.type) {
		case QueryNodeType::SELECT_NODE: {
			auto &select = node.Cast<SelectNode>();
			if (select.from_table) {
				QualifyTableRef(*select.from_table);
			}
			for (auto &expr : select.select_list) {
				QualifyColumn(expr);
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
			// the recursive part sees the CTE itself
			cte_names.push_back(identifier_set_t {recursive.ctename});
			QualifyNode(*recursive.right);
			for (auto &key : recursive.key_targets) {
				QualifyExpression(*key);
			}
			cte_names.pop_back();
			break;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(
		    node, [&](unique_ptr<ParsedExpression> &child) { QualifyExpression(*child); });
		cte_names.pop_back();
	}

	//! Set the qualified names, after the arguments are substituted
	void Apply() {
		// a column without alias keeps the name it has without the qualification, outer expressions first, and a star
		// that an argument can also bring in names its columns itself
		for (auto it = unnamed_expressions.rbegin(); it != unnamed_expressions.rend(); ++it) {
			auto &expr = it->expr.get();
			if (!expr->HasAlias() && (it->name_as_written || !ContainsStar(*expr))) {
				expr->SetAlias(expr->GetName());
			}
		}
		for (auto &change : changes) {
			change.name.get() = std::move(change.qualified);
		}
		unnamed_expressions.clear();
		changes.clear();
	}

private:
	struct UnnamedExpression {
		UnnamedExpression(unique_ptr<ParsedExpression> &expr, bool name_as_written)
		    : expr(expr), name_as_written(name_as_written) {
		}

		reference<unique_ptr<ParsedExpression>> expr;
		bool name_as_written;
	};

	struct NameChange {
		NameChange(QualifiedName &name, QualifiedName qualified) : name(name), qualified(std::move(qualified)) {
		}

		reference<QualifiedName> name;
		QualifiedName qualified;
	};

	//! Qualify an expression whose name can name a column, a star in which names its columns itself unless the name is
	//! taken as it is written
	void QualifyColumn(unique_ptr<ParsedExpression> &expr, bool name_as_written = false) {
		const auto changes_before = changes.size();
		QualifyExpression(*expr);
		if (changes.size() != changes_before && !expr->HasAlias()) {
			unnamed_expressions.emplace_back(expr, name_as_written);
		}
	}

	void QualifyTableRef(TableRef &ref) {
		switch (ref.type) {
		case TableReferenceType::BASE_TABLE: {
			auto &table = ref.Cast<BaseTableRef>();
			if (!IsVisibleCTE(table.Table())) {
				Qualify(CatalogType::TABLE_ENTRY, table.GetQualifiedNameMutable());
			}
			break;
		}
		case TableReferenceType::TABLE_FUNCTION: {
			auto &function = ref.Cast<TableFunctionRef>().function;
			if (function->GetExpressionClass() == ExpressionClass::FUNCTION) {
				auto &function_expr = function->Cast<FunctionExpression>();
				Qualify(CatalogType::TABLE_FUNCTION_ENTRY, function_expr.GetQualifiedNameMutable());
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
		case TableReferenceType::PIVOT: {
			auto &pivot = ref.Cast<PivotRef>();
			QualifyTableRef(*pivot.source);
			for (auto &aggregate : pivot.aggregates) {
				// the columns are named after the aggregates only if there are several
				if (pivot.aggregates.size() > 1) {
					QualifyColumn(aggregate, true);
				} else {
					QualifyExpression(*aggregate);
				}
			}
			for (auto &column : pivot.pivots) {
				for (auto &expr : column.pivot_expressions) {
					QualifyExpression(*expr);
				}
				for (auto &entry : column.entries) {
					if (entry.expr) {
						QualifyExpression(*entry.expr);
					}
				}
				if (column.subquery) {
					QualifyNode(*column.subquery);
				}
			}
			break;
		}
		case TableReferenceType::MATCH_RECOGNIZE: {
			auto &match = ref.Cast<MatchRecognizeRef>();
			if (match.input) {
				QualifyTableRef(*match.input);
			}
			auto &config = *match.config;
			for (auto &expr : config.partition_expressions) {
				QualifyColumn(expr);
			}
			for (auto &order : config.order_by_expressions) {
				QualifyExpression(*order.expression);
			}
			for (auto &expr : config.measures_expression_list) {
				QualifyColumn(expr);
			}
			for (auto &expr : config.defines_expression_list) {
				QualifyExpression(*expr);
			}
			if (config.pattern) {
				QualifyExpression(*config.pattern);
			}
			break;
		}
		case TableReferenceType::SHOW_REF: {
			auto &show = ref.Cast<ShowRef>();
			if (show.query) {
				QualifyNode(*show.query);
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

	static bool ContainsStar(ParsedExpression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::STAR) {
			return true;
		}
		bool result = false;
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](ParsedExpression &child) { result = result || ContainsStar(child); });
		return result;
	}

	bool IsVisibleCTE(const Identifier &name) {
		for (auto &names : cte_names) {
			if (names.count(name)) {
				return true;
			}
		}
		// the query that calls the macro can have a CTE of that name
		auto cte = binder.GetCTEBinding(BindingAlias(name));
		return cte && cte->CanBeReferenced();
	}

	//! Qualify the name with the schema of the entry the macro's search path finds, if the caller's finds another one
	void Qualify(CatalogType type, QualifiedName &name) {
		if (name.Path().size() != 1) {
			return;
		}
		auto macro_entry = LookUp(macro_retriever, type, name);
		if (!macro_entry) {
			return;
		}
		auto caller_entry = LookUp(caller_retriever, type, name);
		if (caller_entry.get() == macro_entry.get()) {
			return;
		}
		// the entry can have another name than the one written, as the default table of a catalog
		auto &schema = macro_entry->ParentSchema();
		changes.emplace_back(name, QualifiedName::FromCatalogSchema(schema.ParentCatalog().GetName(),
		                                                            schema.GetSchemaPath(), macro_entry->name));
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
	//! The names to qualify
	vector<NameChange> changes;
	//! The expressions without alias that name a column and have a name to qualify, inner ones first
	vector<UnnamedExpression> unnamed_expressions;
	//! The CTEs of the body that are visible where the walk is, innermost last
	vector<identifier_set_t> cte_names;
};

} // namespace

void Binder::QualifyMacroBody(StandardEntry &macro, ParsedExpression &body,
                              const std::function<void()> &replace_parameters) {
	MacroBodyQualifier qualifier(*this, entry_retriever, GetMacroSearchPath(macro));
	qualifier.QualifyExpression(body);
	replace_parameters();
	qualifier.Apply();
}

void Binder::QualifyMacroBody(StandardEntry &macro, QueryNode &body, const std::function<void()> &replace_parameters) {
	MacroBodyQualifier qualifier(*this, entry_retriever, GetMacroSearchPath(macro));
	qualifier.QualifyNode(body);
	replace_parameters();
	qualifier.Apply();
}

vector<CatalogSearchEntry> Binder::GetMacroSearchPath(StandardEntry &macro) {
	return GetMacroSearchPath(macro.ParentCatalog(), macro.ParentSchema().GetSchemaPath());
}

vector<CatalogSearchEntry> Binder::GetMacroSearchPath(Catalog &catalog, const vector<Identifier> &schema_path) {
	// a nested schema cannot be in a search path, so the macro's catalog is looked in first
	return GetSearchPath(catalog, schema_path.size() == 1 ? schema_path[0] : Identifier(), true);
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
	QualifyMacroBody(macro_func, *node, [&]() {
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    *node, [&](unique_ptr<ParsedExpression> &child) { eb.ReplaceMacroParameters(child, lambda_params); });
	});

	return node;
}

} // namespace duckdb
