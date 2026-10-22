#include "duckdb/execution/operator/persistent/physical_export.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/dependency_manager.hpp"

#include <algorithm>
#include <sstream>

namespace duckdb {

void ReorderTableEntries(catalog_entry_vector_t &tables);

using duckdb::stringstream;

PhysicalExport::PhysicalExport(PhysicalPlan &physical_plan, vector<LogicalType> types, CopyFunction function,
                               unique_ptr<CopyInfo> info, idx_t estimated_cardinality,
                               unique_ptr<BoundExportData> exported_tables)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXPORT, std::move(types), estimated_cardinality),
      function(std::move(function)), info(std::move(info)), exported_tables(std::move(exported_tables)) {
}

static void WriteCatalogEntries(stringstream &ss, catalog_entry_vector_t &entries) {
	for (auto &entry : entries) {
		if (entry.get().internal) {
			continue;
		}
		auto create_info = entry.get().GetInfo();
		try {
			// the catalog is implied by the database the export is imported into - keep only the schema path
			create_info->StripCatalogQualification();
			auto to_string = create_info->ToString();
			ss << to_string;
		} catch (const NotImplementedException &) {
			ss << entry.get().ToSQL();
		}
		ss << '\n';
	}
	ss << '\n';
}

static void WriteStringStreamToFile(FileSystem &fs, stringstream &ss, const string &path) {
	auto ss_string = ss.str();
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
	                                    FileLockType::WRITE_LOCK);
	fs.Write(*handle, (void *)ss_string.c_str(), NumericCast<int64_t>(ss_string.size()));
	handle.reset();
}

static void WriteCopyStatement(FileSystem &fs, stringstream &ss, CopyInfo &info, ExportedTableData &exported_table) {
	ss << "COPY ";

	//! NOTE: The catalog is explicitly not set here
	auto table_name = exported_table.qualified_name;
	table_name.StripCatalog();
	auto file_path = StringUtil::Replace(exported_table.file_path, "\\", "/");
	ss << StringUtil::Format("%s FROM %s (", table_name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA),
	                         SQLString(file_path));
	// write the copy options
	ss << "FORMAT '" << info.format << "'";
	if (info.format == "csv") {
		info.options.erase("force_not_null");
		for (auto &not_null_column : exported_table.not_null_columns) {
			info.options["force_not_null"].push_back(not_null_column);
		}
		// empty strings are written as quoted empty values, which must not be read back as NULL
		if (info.options.find("allow_quoted_nulls") == info.options.end()) {
			ss << ", allow_quoted_nulls false";
		}
	}
	for (auto &copy_option : info.options) {
		if (copy_option.second.empty()) {
			// empty options are interpreted as TRUE
			copy_option.second.push_back(true);
		}
		ss << ", " << copy_option.first << " ";
		if (copy_option.second.size() == 1) {
			ss << copy_option.second[0].ToSQLString();
		} else {
			// For Lists
			ss << "(";
			for (idx_t i = 0; i < copy_option.second.size(); i++) {
				ss << copy_option.second[i].ToSQLString();
				if (i != copy_option.second.size() - 1) {
					ss << ", ";
				}
			}
			ss << ")";
		}
	}
	ss << ");" << '\n';
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class ExportSourceState : public GlobalSourceState {
public:
	ExportSourceState() : finished(false) {
	}

	bool finished;
};

unique_ptr<GlobalSourceState> PhysicalExport::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<ExportSourceState>();
}

void PhysicalExport::ExtractEntries(ClientContext &context, vector<reference<SchemaCatalogEntry>> &schema_list,
                                    ExportEntries &result) {
	for (auto &schema_p : schema_list) {
		auto &schema = schema_p.get();
		auto &catalog = schema.ParentCatalog();
		if (catalog.IsSystemCatalog() || catalog.IsTemporaryCatalog()) {
			continue;
		}
		if (!schema.internal) {
			result.schemas.push_back(schema);
		}
		schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			if (entry.type != CatalogType::TABLE_ENTRY) {
				result.views.push_back(entry);
			}
			if (entry.type == CatalogType::TABLE_ENTRY) {
				result.tables.push_back(entry);
			}
		});
		schema.Scan(context, CatalogType::SEQUENCE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			result.sequences.push_back(entry);
		});
		schema.Scan(context, CatalogType::TYPE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			result.custom_types.push_back(entry);
		});
		schema.Scan(context, CatalogType::INDEX_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			result.indexes.push_back(entry);
		});
		schema.Scan(context, CatalogType::MACRO_ENTRY, [&](CatalogEntry &entry) {
			if (!entry.internal && entry.type == CatalogType::MACRO_ENTRY) {
				result.macros.push_back(entry);
			}
		});
		schema.Scan(context, CatalogType::TABLE_MACRO_ENTRY, [&](CatalogEntry &entry) {
			if (!entry.internal && entry.type == CatalogType::TABLE_MACRO_ENTRY) {
				result.macros.push_back(entry);
			}
		});
	}
}

//! Finds the names of the tables and views that binding a query reads. A name that refers to a common table
//! expression is not one, and a common table expression is only bound when the query uses it.
class QueryReadFinder {
public:
	vector<reference<const QualifiedName>> reads;

public:
	void VisitNode(QueryNode &node) {
		scopes.push_back(CTEScope {node, node.cte_map.map.size()});
		switch (node.type) {
		case QueryNodeType::SELECT_NODE: {
			auto &select = node.Cast<SelectNode>();
			if (select.from_table) {
				VisitTableRef(*select.from_table);
			}
			for (auto &expr : select.select_list) {
				VisitNodeExpression(*expr);
			}
			for (auto &expr : select.groups.group_expressions) {
				VisitNodeExpression(*expr);
			}
			if (select.where_clause) {
				VisitNodeExpression(*select.where_clause);
			}
			if (select.having) {
				VisitNodeExpression(*select.having);
			}
			if (select.qualify) {
				VisitNodeExpression(*select.qualify);
			}
			break;
		}
		case QueryNodeType::SET_OPERATION_NODE:
			for (auto &child : node.Cast<SetOperationNode>().children) {
				VisitNode(*child);
			}
			break;
		case QueryNodeType::RECURSIVE_CTE_NODE: {
			auto &recursive = node.Cast<RecursiveCTENode>();
			VisitNode(*recursive.left);
			VisitNode(*recursive.right);
			break;
		}
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(
		    node, [&](unique_ptr<ParsedExpression> &child) { VisitNodeExpression(*child); });
		scopes.pop_back();
	}

private:
	//! The common table expressions of a query node that a name can refer to
	struct CTEScope {
		reference<QueryNode> node;
		idx_t visible_count;
	};
	vector<CTEScope> scopes;
	reference_set_t<CommonTableExpressionInfo> bound_ctes;

private:
	void VisitTableRef(TableRef &ref) {
		switch (ref.type) {
		case TableReferenceType::BASE_TABLE:
			VisitName(ref.Cast<BaseTableRef>().GetQualifiedName());
			break;
		case TableReferenceType::SUBQUERY:
			VisitNode(*ref.Cast<SubqueryRef>().subquery->node);
			break;
		case TableReferenceType::JOIN: {
			auto &join = ref.Cast<JoinRef>();
			VisitTableRef(*join.left);
			VisitTableRef(*join.right);
			if (join.condition) {
				VisitNodeExpression(*join.condition);
			}
			break;
		}
		case TableReferenceType::EXPRESSION_LIST:
			for (auto &row : ref.Cast<ExpressionListRef>().values) {
				for (auto &expr : row) {
					VisitNodeExpression(*expr);
				}
			}
			break;
		case TableReferenceType::PIVOT:
			VisitTableRef(*ref.Cast<PivotRef>().source);
			break;
		default:
			// e.g. the arguments of a table function, which a table macro does not have to use
			break;
		}
	}

	void VisitExpression(ParsedExpression &expr) {
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::SUBQUERY: {
			auto &subquery = expr.Cast<SubqueryExpression>();
			VisitNode(*subquery.SubqueryMutable()->node);
			if (subquery.GetChildMutable()) {
				VisitExpression(*subquery.GetChildMutable());
			}
			break;
		}
		case ExpressionClass::BETWEEN:
		case ExpressionClass::CASE:
		case ExpressionClass::CAST:
		case ExpressionClass::COLLATE:
		case ExpressionClass::COMPARISON:
		case ExpressionClass::CONJUNCTION:
		case ExpressionClass::OPERATOR:
			ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) { VisitExpression(child); });
			break;
		default:
			// e.g. the arguments of a function, which a macro does not have to use
			break;
		}
	}

	//! Visits an expression of the query node, unless it holds a star, which can expand to no expression at all
	void VisitNodeExpression(ParsedExpression &expr) {
		bool has_star = false;
		ParsedExpressionIterator::VisitExpressionClass(expr, ExpressionClass::STAR,
		                                               [&](const ParsedExpression &) { has_star = true; });
		if (!has_star) {
			VisitExpression(expr);
		}
	}

	void VisitName(const QualifiedName &name) {
		for (idx_t scope_index = scopes.size(); scope_index > 0; scope_index--) {
			auto &scope = scopes[scope_index - 1];
			auto &ctes = scope.node.get().cte_map.map;
			auto cte = ctes.find(name.Name());
			if (cte == ctes.end() || NumericCast<idx_t>(cte - ctes.begin()) >= scope.visible_count) {
				continue;
			}
			if (name.Path().size() == 1) {
				BindCTE(scope_index - 1, NumericCast<idx_t>(cte - ctes.begin()));
			}
			// a qualified name can refer to the state of a recursive common table expression, e.g. recurring.r
			return;
		}
		reads.push_back(name);
	}

	void BindCTE(idx_t scope_index, idx_t cte_index) {
		auto &cte = *(scopes[scope_index].node.get().cte_map.map.begin() + NumericCast<int64_t>(cte_index))->second;
		if (!cte.query_node || !bound_ctes.insert(cte).second) {
			return;
		}
		// the common table expression sees the ones before it, and itself when it is recursive
		auto outer_scopes = scopes;
		scopes.erase(scopes.begin() + NumericCast<int64_t>(scope_index + 1), scopes.end());
		scopes.back().visible_count = cte_index;
		if (cte.query_node->type == QueryNodeType::RECURSIVE_CTE_NODE) {
			scopes.back().visible_count++;
		}
		VisitNode(*cte.query_node);
		scopes = std::move(outer_scopes);
	}
};

//! The key of a table or view by its schema path and name
static Identifier RelationKey(const vector<Identifier> &schema_path, const Identifier &name) {
	string key;
	for (auto &schema : schema_path) {
		key += schema.GetIdentifierName();
		key += '\0';
	}
	key += name.GetIdentifierName();
	return Identifier(std::move(key));
}

//! Adds the exported view that a name in a view refers to. The relations map the key of every exported table and
//! view to the index of the view among the entries, invalid for a table.
static void AddViewReads(ViewCatalogEntry &view, const QualifiedName &name,
                         const identifier_map_t<optional_idx> &relations, vector<idx_t> &reads) {
	auto &catalog = view.ParentCatalog();
	auto &catalog_name = catalog.GetName();
	vector<Identifier> qualification(name.Path().begin(), name.Path().end() - 1);
	auto add_schema_reads = [&](const vector<Identifier> &schema_path) {
		auto relation = relations.find(RelationKey(schema_path, name.Name()));
		if (relation == relations.end()) {
			return false;
		}
		if (relation->second.IsValid()) {
			reads.push_back(relation->second.GetIndex());
		}
		return true;
	};
	auto default_schema = catalog.GetDefaultSchema();
	if (qualification.empty()) {
		auto view_schema_path = view.ParentSchema().GetSchemaPath();
		if (view_schema_path.size() > 1) {
			// the binder looks a name in a nested schema up by the last name of the schema
			return;
		}
		// a name without a schema refers to the schema of the view first, then to the default schema
		auto in_view_schema = relations.find(RelationKey(view_schema_path, name.Name()));
		auto in_default_schema = relations.end();
		if (default_schema && view_schema_path != vector<Identifier> {*default_schema}) {
			in_default_schema = relations.find(RelationKey(vector<Identifier> {*default_schema}, name.Name()));
		}
		if (in_view_schema == relations.end()) {
			if (in_default_schema != relations.end() && in_default_schema->second.IsValid()) {
				reads.push_back(in_default_schema->second.GetIndex());
			}
		} else if (in_view_schema->second.IsValid() && in_default_schema == relations.end()) {
			// while a view of the schema of the view does not exist yet, binding falls back to the default schema
			reads.push_back(in_view_schema->second.GetIndex());
		}
		return;
	}
	if (add_schema_reads(qualification) || qualification[0] != catalog_name) {
		return;
	}
	// the qualification can start with the name of the catalog
	qualification.erase(qualification.begin());
	if (!qualification.empty()) {
		add_schema_reads(qualification);
	} else if (default_schema) {
		add_schema_reads(vector<Identifier> {*default_schema});
	}
}

//! Moves the views of the export order after the views they read by name (view dependencies are only recorded with
//! enable_view_dependencies). The order already respects everything else an entry needs on import.
static void ReorderViewEntries(ClientContext &context, catalog_entry_vector_t &entries) {
	// On import, a name without a catalog can also refer to a temporary or a built in table or view, first or while
	// the exported one does not exist yet, so it adds no read
	identifier_set_t fallback_names;
	identifier_set_t fallback_relations;
	for (auto &catalog_name : {TEMP_CATALOG, SYSTEM_CATALOG}) {
		for (auto &schema : Catalog::GetCatalog(context, catalog_name).GetSchemas(context)) {
			auto schema_path = schema.get().GetSchemaPath();
			schema.get().Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
				fallback_names.insert(entry.name);
				fallback_relations.insert(RelationKey(schema_path, entry.name));
			});
		}
	}
	identifier_map_t<optional_idx> relations;
	for (idx_t i = 0; i < entries.size(); i++) {
		auto &entry = entries[i].get();
		if (entry.type != CatalogType::TABLE_ENTRY && entry.type != CatalogType::VIEW_ENTRY) {
			continue;
		}
		auto view_index = entry.type == CatalogType::VIEW_ENTRY ? optional_idx(i) : optional_idx();
		relations.emplace(RelationKey(entry.ParentSchema().GetSchemaPath(), entry.name), view_index);
	}
	vector<vector<idx_t>> reads(entries.size());
	for (idx_t i = 0; i < entries.size(); i++) {
		auto &entry = entries[i].get();
		if (entry.type != CatalogType::VIEW_ENTRY) {
			continue;
		}
		auto &view = entry.Cast<ViewCatalogEntry>();
		if (!view.query) {
			continue;
		}
		QueryReadFinder finder;
		finder.VisitNode(*view.query->node);
		for (auto &read : finder.reads) {
			auto &name = read.get();
			vector<Identifier> qualification(name.Path().begin(), name.Path().end() - 1);
			if (qualification.empty() ? fallback_names.count(name.Name()) > 0
			                          : fallback_relations.count(RelationKey(qualification, name.Name())) > 0) {
				continue;
			}
			AddViewReads(view, name, relations, reads[i]);
		}
		auto &view_reads = reads[i];
		view_reads.erase(std::remove(view_reads.begin(), view_reads.end(), i), view_reads.end());
		std::sort(view_reads.begin(), view_reads.end());
		view_reads.erase(std::unique(view_reads.begin(), view_reads.end()), view_reads.end());
	}
	// Depth first in the current order. A view only moves before a view that reads it, and what it needs comes before
	// it in the current order, so it is written first. That keeps an order that works working, as long as every read
	// found here is one that binding the view on import makes.
	struct EntryVisit {
		idx_t entry;
		idx_t next_read;
	};
	catalog_entry_vector_t result;
	result.reserve(entries.size());
	vector<bool> visited(entries.size(), false);
	vector<EntryVisit> stack;
	for (idx_t i = 0; i < entries.size(); i++) {
		if (visited[i]) {
			continue;
		}
		visited[i] = true;
		stack.push_back(EntryVisit {i, 0});
		while (!stack.empty()) {
			auto entry = stack.back().entry;
			auto next_read = stack.back().next_read;
			if (next_read == reads[entry].size()) {
				result.push_back(entries[entry]);
				stack.pop_back();
				continue;
			}
			stack.back().next_read++;
			auto read = reads[entry][next_read];
			if (!visited[read]) {
				visited[read] = true;
				stack.push_back(EntryVisit {read, 0});
			}
		}
	}
	entries = std::move(result);
}

static void AddEntries(catalog_entry_vector_t &all_entries, catalog_entry_vector_t &to_add) {
	for (auto &entry : to_add) {
		all_entries.push_back(entry);
	}
	to_add.clear();
}

catalog_entry_vector_t PhysicalExport::GetNaiveExportOrder(ClientContext &context, Catalog &catalog) {
	// gather all catalog types to export
	ExportEntries entries;
	auto schema_list = catalog.GetSchemas(context);
	PhysicalExport::ExtractEntries(context, schema_list, entries);

	ReorderTableEntries(entries.tables);

	// order macro's by timestamp so nested macro's are imported nicely
	sort(entries.macros.begin(), entries.macros.end(),
	     [](const reference<CatalogEntry> &lhs, const reference<CatalogEntry> &rhs) {
		     return lhs.get().oid < rhs.get().oid;
	     });

	catalog_entry_vector_t catalog_entries;
	idx_t size = 0;
	size += entries.schemas.size();
	size += entries.custom_types.size();
	size += entries.sequences.size();
	size += entries.tables.size();
	size += entries.views.size();
	size += entries.indexes.size();
	size += entries.macros.size();
	catalog_entries.reserve(size);
	AddEntries(catalog_entries, entries.schemas);
	AddEntries(catalog_entries, entries.sequences);
	AddEntries(catalog_entries, entries.custom_types);
	AddEntries(catalog_entries, entries.tables);
	AddEntries(catalog_entries, entries.macros);
	AddEntries(catalog_entries, entries.views);
	AddEntries(catalog_entries, entries.indexes);
	return catalog_entries;
}

SourceResultType PhysicalExport::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<ExportSourceState>();
	if (state.finished) {
		return SourceResultType::FINISHED;
	}

	auto &ccontext = context.client;
	auto &fs = FileSystem::GetFileSystem(ccontext);

	auto &catalog = Catalog::GetCatalog(ccontext, info->GetQualifiedName().Catalog());

	catalog_entry_vector_t catalog_entries;
	catalog_entries = GetNaiveExportOrder(context.client, catalog);
	auto dependency_manager = catalog.GetDependencyManager();
	if (dependency_manager) {
		dependency_manager->ReorderEntries(catalog_entries, ccontext);
	}
	ReorderViewEntries(ccontext, catalog_entries);

	// write the schema.sql file
	stringstream ss;
	WriteCatalogEntries(ss, catalog_entries);
	WriteStringStreamToFile(fs, ss, fs.JoinPath(info->file_path, "schema.sql"));

	// write the load.sql file
	// for every table, we write COPY INTO statement with the specified options
	stringstream load_ss;
	for (idx_t i = 0; i < exported_tables->data.size(); i++) {
		auto exported_table_info = exported_tables->data[i].table_data;
		WriteCopyStatement(fs, load_ss, *info, exported_table_info);
	}
	WriteStringStreamToFile(fs, load_ss, fs.JoinPath(info->file_path, "load.sql"));
	state.finished = true;

	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType PhysicalExport::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	// nop
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalExport::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	// EXPORT has an optional child
	// we only need to schedule child pipelines if there is a child
	auto &state = meta_pipeline.GetState();
	state.SetPipelineSource(current, *this);
	if (children.empty()) {
		return;
	}
	PhysicalOperator::BuildPipelines(current, meta_pipeline);
}

vector<const_reference<PhysicalOperator>> PhysicalExport::GetSources() const {
	return {*this};
}

} // namespace duckdb
