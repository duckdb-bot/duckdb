//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/rollback_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/undo_buffer.hpp"

namespace duckdb {
class ClientContext;
class DataChunk;
class DataTable;
class DuckTransaction;
class WriteAheadLog;

class RollbackState {
public:
	RollbackState(DuckTransaction &transaction, ClientContext &context);

public:
	void RollbackEntry(UndoFlags type, data_ptr_t data);

private:
	DuckTransaction &transaction;
	//! The connection that rolls back, for index key expressions that need a client context
	ClientContext &context;
};

} // namespace duckdb
