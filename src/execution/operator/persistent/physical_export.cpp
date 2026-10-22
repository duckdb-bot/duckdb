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
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
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

//! Adds the names of the common table expressions of a query node and of the query nodes it is made of
static void AddCommonTableExpressionNames(QueryNode &node, identifier_set_t &names) {
	for (auto &cte : node.cte_map.map) {
		names.insert(cte.first);
		if (cte.second->query_node) {
			AddCommonTableExpressionNames(*cte.second->query_node, names);
		}
	}
	if (node.type == QueryNodeType::SET_OPERATION_NODE) {
		for (auto &child : node.Cast<SetOperationNode>().children) {
			AddCommonTableExpressionNames(*child, names);
		}
	} else if (node.type == QueryNodeType::RECURSIVE_CTE_NODE) {
		auto &recursive_node = node.Cast<RecursiveCTENode>();
		AddCommonTableExpressionNames(*recursive_node.left, names);
		AddCommonTableExpressionNames(*recursive_node.right, names);
	}
}

//! Collects the tables and views a query reads by name, and the names of its common table expressions
static void CollectQueryReads(QueryNode &node, vector<reference<BaseTableRef>> &tables, identifier_set_t &cte_names) {
	AddCommonTableExpressionNames(node, cte_names);
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node,
	    [&](unique_ptr<ParsedExpression> &child) {
		    ParsedExpressionIterator::VisitExpressionClassMutable(
		        *child, ExpressionClass::SUBQUERY, [&](ParsedExpression &subquery) {
			        CollectQueryReads(*subquery.Cast<SubqueryExpression>().SubqueryMutable()->node, tables, cte_names);
		        });
	    },
	    [&](TableRef &ref) {
		    if (ref.type == TableReferenceType::BASE_TABLE) {
			    tables.push_back(ref.Cast<BaseTableRef>());
		    } else if (ref.type == TableReferenceType::SUBQUERY) {
			    AddCommonTableExpressionNames(*ref.Cast<SubqueryRef>().subquery->node, cte_names);
		    }
	    });
}

//! An exported table or view, found by its name
struct ExportedRelation {
	reference<CatalogEntry> entry;
	//! The index among the exported views, invalid for a table
	optional_idx view_index;
};

//! Adds the exported views of the given name that a name in a view refers to
static void AddViewReads(ViewCatalogEntry &view, const QualifiedName &name, const vector<ExportedRelation> &relations,
                         vector<idx_t> &reads) {
	auto &catalog = view.ParentCatalog();
	auto &catalog_name = catalog.GetName();
	if (!name.Catalog().empty() && name.Catalog() != catalog_name) {
		return;
	}
	auto default_schema = catalog.GetDefaultSchema();
	auto add_schema_reads = [&](const Identifier &schema_name) {
		bool found = false;
		for (auto &relation : relations) {
			if (relation.entry.get().ParentSchema().name != schema_name) {
				continue;
			}
			found = true;
			if (relation.view_index.IsValid()) {
				reads.push_back(relation.view_index.GetIndex());
			}
		}
		return found;
	};
	if (!name.Schema().empty()) {
		// the schema name can also be the name of the catalog
		if (!add_schema_reads(name.Schema()) && name.Catalog().empty() && name.Schema() == catalog_name &&
		    default_schema) {
			add_schema_reads(*default_schema);
		}
		return;
	}
	// a name without a schema refers to the schema of the view first, then to the default schema
	if (!add_schema_reads(view.ParentSchema().name) && default_schema) {
		add_schema_reads(*default_schema);
	}
}

//! Orders the views so that every view comes after the views it reads. View dependencies are only recorded with
//! enable_view_dependencies, so the names in the queries of the views are looked up among the exported relations.
static void ReorderViewEntries(const catalog_entry_vector_t &tables, catalog_entry_vector_t &views) {
	identifier_map_t<vector<ExportedRelation>> relations_by_name;
	for (auto &table : tables) {
		relations_by_name[table.get().name].push_back(ExportedRelation {table, optional_idx()});
	}
	for (idx_t i = 0; i < views.size(); i++) {
		relations_by_name[views[i].get().name].push_back(ExportedRelation {views[i], optional_idx(i)});
	}
	vector<vector<idx_t>> reads(views.size());
	for (idx_t i = 0; i < views.size(); i++) {
		auto &entry = views[i].get();
		if (entry.type != CatalogType::VIEW_ENTRY) {
			continue;
		}
		auto &view = entry.Cast<ViewCatalogEntry>();
		if (!view.query) {
			continue;
		}
		vector<reference<BaseTableRef>> tables;
		identifier_set_t cte_names;
		CollectQueryReads(*view.query->node, tables, cte_names);
		for (auto &table : tables) {
			auto &name = table.get().GetQualifiedName();
			if (name.Catalog().empty() && name.Schema().empty() && cte_names.count(name.Name())) {
				continue;
			}
			auto relations = relations_by_name.find(name.Name());
			if (relations != relations_by_name.end()) {
				AddViewReads(view, name, relations->second, reads[i]);
			}
		}
		auto &view_reads = reads[i];
		view_reads.erase(std::remove(view_reads.begin(), view_reads.end(), i), view_reads.end());
		std::sort(view_reads.begin(), view_reads.end());
		view_reads.erase(std::unique(view_reads.begin(), view_reads.end()), view_reads.end());
	}
	// depth first in the current order, so views that read no other view keep their order
	struct ViewVisit {
		idx_t view;
		idx_t next_read;
	};
	catalog_entry_vector_t result;
	result.reserve(views.size());
	vector<bool> visited(views.size(), false);
	vector<ViewVisit> stack;
	for (idx_t i = 0; i < views.size(); i++) {
		if (visited[i]) {
			continue;
		}
		visited[i] = true;
		stack.push_back(ViewVisit {i, 0});
		while (!stack.empty()) {
			auto view = stack.back().view;
			auto next_read = stack.back().next_read;
			if (next_read == reads[view].size()) {
				result.push_back(views[view]);
				stack.pop_back();
				continue;
			}
			stack.back().next_read++;
			auto read = reads[view][next_read];
			if (!visited[read]) {
				visited[read] = true;
				stack.push_back(ViewVisit {read, 0});
			}
		}
	}
	views = std::move(result);
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
	ReorderViewEntries(entries.tables, entries.views);

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
