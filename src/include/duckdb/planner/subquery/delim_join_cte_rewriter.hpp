//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/subquery/delim_join_cte_rewriter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class LogicalMaterializedCTE;

//! Rewrites fully decorrelated DelimJoins into materialized CTEs.
class DelimJoinCTERewriter {
public:
	static void Rewrite(Binder &binder, unique_ptr<LogicalOperator> &plan);
	//! Whether a materialized CTE is the domain or the dedup CTE of a rewritten delim join, by its generated name
	static bool IsGeneratedDomainCTE(const LogicalMaterializedCTE &cte);
	static bool IsGeneratedDedupCTE(const LogicalMaterializedCTE &cte);

private:
	explicit DelimJoinCTERewriter(Binder &binder);

	//! The generated CTE names carry the table index of the CTE they name
	static Identifier DomainCTEName(TableIndex cte_index);
	static Identifier DedupCTEName(TableIndex dedup_cte_index);

	void Rewrite(unique_ptr<LogicalOperator> &plan);
	BindingReplacementGraph RewriteDelimJoinsToCTEs(unique_ptr<LogicalOperator> &plan, LogicalOperator &rewrite_root,
	                                                bool null_rejecting_filter_above = false,
	                                                bool preserve_evidence_side = false);
	BindingReplacementGraph MaterializeDelimJoinAsCTE(unique_ptr<LogicalOperator> &plan, LogicalOperator &rewrite_root,
	                                                  bool null_rejecting_filter_above, bool preserve_evidence_side,
	                                                  bool preserve_nested_evidence_side);

private:
	Binder &binder;
	bool cte_deliminator_enabled;
	vector<TableIndex> generated_dedup_cte_indexes;
	set<TableIndex> marker_indexes;
	set<TableIndex> preserve_marker_indexes;
	set<TableIndex> preserve_nested_marker_indexes;
};

} // namespace duckdb
