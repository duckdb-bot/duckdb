#include "duckdb/common/types/string_heap.hpp"

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/query_location.hpp"
#include "utf8proc_wrapper.hpp"

#include <cstring>

namespace duckdb {

static constexpr const char *MAXIMUM_STRING_SIZE_ERROR = "MAXIMUM_STRING_SIZE";

StringHeap::StringHeap(Allocator &allocator) : allocator(allocator) {
}

void StringHeap::ThrowMaximumSizeExceeded(idx_t len) {
	auto extra_info = Exception::InitializeExtraInfo(MAXIMUM_STRING_SIZE_ERROR, QueryLocation());
	throw Exception(
	    extra_info, ExceptionType::OUT_OF_RANGE,
	    Exception::ConstructMessage("Cannot create a string of size: '%d', the maximum supported string size is: '%d'",
	                                len, string_t::MAX_STRING_SIZE));
}

bool StringHeap::IsMaximumSizeError(const ErrorData &error) {
	auto &extra_info = error.ExtraInfo();
	auto entry = extra_info.find("error_subtype");
	return entry != extra_info.end() && entry->second == MAXIMUM_STRING_SIZE_ERROR;
}

void StringHeap::Destroy() {
	allocator.Destroy();
}

void StringHeap::Move(StringHeap &other) {
	other.allocator.Move(allocator);
}

idx_t StringHeap::SizeInBytes() const {
	return allocator.SizeInBytes();
}

idx_t StringHeap::AllocationSize() const {
	return allocator.AllocationSize();
}

} // namespace duckdb
