//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/collation_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! ALL_COLLATIONS          -> all collations, so that values compare and sort as their type defines
//! COMBINABLE_COLLATIONS   -> only the combinable string collations
//! HASH_COLLATIONS         -> only the collations values that compare equal need to hash equally, without string
//!                            collations or keys of types that already hash by value
enum class CollationType { ALL_COLLATIONS, COMBINABLE_COLLATIONS, HASH_COLLATIONS };

} // namespace duckdb
