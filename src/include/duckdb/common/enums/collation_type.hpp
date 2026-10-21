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
//! NORMALIZING_COLLATIONS  -> only the collations that give values that compare equal one representation, without
//!                            string collations or keys that only change the order
enum class CollationType { ALL_COLLATIONS, COMBINABLE_COLLATIONS, NORMALIZING_COLLATIONS };

} // namespace duckdb
