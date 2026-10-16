#include "duckdb/function/table/range.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {

struct CheckpointBindData : public FunctionData {
	explicit CheckpointBindData(Identifier db_name) : db_name(std::move(db_name)) {
	}

	//! The database is looked up again when executing, as a prepared statement can outlive it
	Identifier db_name;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CheckpointBindData>(db_name);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CheckpointBindData>();
		return db_name == other.db_name;
	}
};

static optional_ptr<AttachedDatabase> GetCheckpointDatabase(ClientContext &context, const Identifier &db_name) {
	auto db = DatabaseManager::Get(context).GetDatabase(context, db_name);
	if (!db) {
		throw BinderException("Database \"%s\" not found", db_name.GetIdentifierName());
	}
	return db;
}

static unique_ptr<FunctionData> CheckpointBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");

	Identifier db_name;
	if (!input.inputs.empty()) {
		if (input.inputs[0].IsNull()) {
			throw BinderException("Database cannot be NULL");
		}
		db_name = Identifier(StringValue::Get(input.inputs[0]));
	} else {
		db_name = DatabaseManager::GetDefaultDatabase(context);
	}
	GetCheckpointDatabase(context, db_name);
	return make_uniq<CheckpointBindData>(std::move(db_name));
}

template <bool FORCE>
static void TemplatedCheckpointFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<CheckpointBindData>();
	auto db = GetCheckpointDatabase(context, bind_data.db_name);
	auto &transaction_manager = TransactionManager::Get(*db);
	transaction_manager.Checkpoint(context, FORCE);
}

void CheckpointFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunctionSet checkpoint("checkpoint");
	checkpoint.AddFunction(TableFunction({}, TemplatedCheckpointFunction<false>, CheckpointBind));
	checkpoint.AddFunction(TableFunction(FunctionSignature().AddPositionalOnly("database", LogicalType::VARCHAR),
	                                     TemplatedCheckpointFunction<false>, CheckpointBind));
	set.AddFunction(checkpoint);

	TableFunctionSet force_checkpoint("force_checkpoint");
	force_checkpoint.AddFunction(TableFunction({}, TemplatedCheckpointFunction<true>, CheckpointBind));
	force_checkpoint.AddFunction(TableFunction(FunctionSignature().AddPositionalOnly("database", LogicalType::VARCHAR),
	                                           TemplatedCheckpointFunction<true>, CheckpointBind));
	set.AddFunction(force_checkpoint);
}

} // namespace duckdb
