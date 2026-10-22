#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

static void HashFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	args.Hash(result);
}

//! Replaces each VARIANT by its comparison key, so that VARIANT values that compare equal hash equally
static unique_ptr<FunctionData> HashBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	for (auto &argument : input.GetArguments()) {
		ExpressionBinder::PushCollation(context, argument, argument->GetReturnType(), CollationType::HASH_COLLATIONS);
	}
	return nullptr;
}

ScalarFunction HashFun::GetFunction() {
	auto hash_fun = ScalarFunction({}, LogicalType::HASH, HashFunction, HashBind);
	hash_fun.GetSignature().AddParameter("value", LogicalType::ANY);
	hash_fun.GetSignature().AddArgs("args", LogicalType::ANY);
	hash_fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return hash_fun;
}

} // namespace duckdb
