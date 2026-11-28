#include "column_writer.hpp"
#include "duckdb/common/types/decimal.hpp"

#include <cmath>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "duckdb/common/types/interval_no_normalize.hpp"
#include "parquet_writer.hpp"
#include "writer/array_column_writer.hpp"
#include "writer/boolean_column_writer.hpp"
#include "writer/decimal_column_writer.hpp"
#include "writer/enum_column_writer.hpp"
#include "writer/list_column_writer.hpp"
#include "writer/struct_column_writer.hpp"
#include "writer/variant_column_writer.hpp"
#include "writer/templated_column_writer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "brotli/encode.h"
#include "lz4.hpp"
#include "miniz_wrapper.hpp"
#include "snappy.h"
#include "zstd.h"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/double_na_equal.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "miniz.hpp"
#include "parquet_field_id.hpp"
#include "parquet_shredding.hpp"
#include "parquet_timestamp.hpp"
#include "parquet_types.h"
#include "writer/parquet_write_operators.hpp"
#include "writer/parquet_write_stats.hpp"

namespace duckdb {
class ClientContext;
struct GeometryStatsData;

using namespace duckdb_parquet; // NOLINT
using namespace duckdb_miniz;   // NOLINT

using duckdb_parquet::CompressionCodec;
using duckdb_parquet::ConvertedType;
using duckdb_parquet::Encoding;
using duckdb_parquet::FieldRepetitionType;
using duckdb_parquet::FileMetaData;
using duckdb_parquet::PageHeader;
using duckdb_parquet::PageType;
using ParquetRowGroup = duckdb_parquet::RowGroup;
using duckdb_parquet::Type;

constexpr uint16_t ColumnWriter::PARQUET_DEFINE_VALID;

//===--------------------------------------------------------------------===//
// ColumnWriterStatistics
//===--------------------------------------------------------------------===//
ColumnWriterStatistics::~ColumnWriterStatistics() {
}

bool ColumnWriterStatistics::HasStats() {
	return false;
}

string ColumnWriterStatistics::GetMin() {
	return string();
}

string ColumnWriterStatistics::GetMax() {
	return string();
}

string ColumnWriterStatistics::GetMinValue() {
	return string();
}

string ColumnWriterStatistics::GetMaxValue() {
	return string();
}

bool ColumnWriterStatistics::CanHaveNaN() {
	return false;
}

bool ColumnWriterStatistics::HasNaN() {
	return false;
}

idx_t ColumnWriterStatistics::GetNaNCount() {
	return 0;
}

bool ColumnWriterStatistics::MinIsExact() {
	return true;
}

bool ColumnWriterStatistics::MaxIsExact() {
	return true;
}

bool ColumnWriterStatistics::HasGeoStats() {
	return false;
}

optional_ptr<GeometryStatsData> ColumnWriterStatistics::GetGeoStats() {
	return nullptr;
}

void ColumnWriterStatistics::WriteGeoStats(duckdb_parquet::GeospatialStatistics &stats) {
	D_ASSERT(false); // this should never be called
}

void ColumnWriterStatistics::Merge(const ColumnWriterStatistics &other) {
	// an accumulator that tracks statistics must fold them in, or the column chunk statistics would be lost
	D_ASSERT(!HasStats());
}

int ColumnWriterStatistics::CompareMin(const ColumnWriterStatistics &other) const {
	return 0;
}

int ColumnWriterStatistics::CompareMax(const ColumnWriterStatistics &other) const {
	return 0;
}

//===--------------------------------------------------------------------===//
// ColumnWriter
//===--------------------------------------------------------------------===//
ColumnWriter::ColumnWriter(ParquetWriter &writer, ParquetColumnSchema &&column_schema_p,
                           vector<Identifier> schema_path_p)
    : writer(writer), column_schema(std::move(column_schema_p)), schema_path(std::move(schema_path_p)) {
	can_have_nulls = column_schema.repetition_type == duckdb_parquet::FieldRepetitionType::OPTIONAL;
}
ColumnWriter::~ColumnWriter() {
}

void ColumnWriter::MarkRepetitionRequired() {
	if (column_schema.repetition_type == duckdb_parquet::FieldRepetitionType::REQUIRED) {
		return;
	}
	D_ASSERT(column_schema.repetition_type == duckdb_parquet::FieldRepetitionType::OPTIONAL);
	D_ASSERT(can_have_nulls);
	column_schema.repetition_type = duckdb_parquet::FieldRepetitionType::REQUIRED;
	can_have_nulls = false;
	DecrementMaxDefineRecursive();
}

void ColumnWriter::DecrementMaxDefineRecursive() {
	D_ASSERT(column_schema.max_define > 0);
	column_schema.max_define--;
	for (auto &child : child_writers) {
		child->DecrementMaxDefineRecursive();
	}
}

bool ColumnWriter::TryExportPreparedShreddingType(ShreddingType &result) const {
	bool has_shredding = false;
	auto writer_shredding_type = ShreddingType(Type());
	for (auto &child_writer : ChildWriters()) {
		ShreddingType child_shredding_type;
		if (!child_writer->TryExportPreparedShreddingType(child_shredding_type)) {
			continue;
		}
		writer_shredding_type.AddChild(Identifier(child_writer->Schema().name), std::move(child_shredding_type));
		has_shredding = true;
	}
	if (!has_shredding) {
		return false;
	}
	result = std::move(writer_shredding_type);
	return true;
}

ColumnWriterState::~ColumnWriterState() {
}

void ColumnWriter::CompressPage(MemoryStream &temp_writer, size_t &compressed_size, data_ptr_t &compressed_data,
                                AllocatedData &compressed_buf) {
	switch (writer.GetCodec()) {
	case CompressionCodec::UNCOMPRESSED:
		compressed_size = temp_writer.GetPosition();
		compressed_data = temp_writer.GetData();
		break;

	case CompressionCodec::SNAPPY: {
		compressed_size = duckdb_snappy::MaxCompressedLength(temp_writer.GetPosition());
		compressed_buf = BufferAllocator::Get(writer.GetContext()).Allocate(compressed_size);
		duckdb_snappy::RawCompress(const_char_ptr_cast(temp_writer.GetData()), temp_writer.GetPosition(),
		                           char_ptr_cast(compressed_buf.get()), &compressed_size);
		compressed_data = compressed_buf.get();
		D_ASSERT(compressed_size <= duckdb_snappy::MaxCompressedLength(temp_writer.GetPosition()));
		break;
	}
	case CompressionCodec::LZ4_RAW: {
		compressed_size = duckdb_lz4::LZ4_compressBound(UnsafeNumericCast<int32_t>(temp_writer.GetPosition()));
		compressed_buf = BufferAllocator::Get(writer.GetContext()).Allocate(compressed_size);
		compressed_size = duckdb_lz4::LZ4_compress_default(
		    const_char_ptr_cast(temp_writer.GetData()), char_ptr_cast(compressed_buf.get()),
		    UnsafeNumericCast<int32_t>(temp_writer.GetPosition()), UnsafeNumericCast<int32_t>(compressed_size));
		compressed_data = compressed_buf.get();
		break;
	}
	case CompressionCodec::GZIP: {
		MiniZStream s;
		compressed_size = s.MaxCompressedLength(temp_writer.GetPosition());
		compressed_buf = BufferAllocator::Get(writer.GetContext()).Allocate(compressed_size);
		s.Compress(const_char_ptr_cast(temp_writer.GetData()), temp_writer.GetPosition(),
		           char_ptr_cast(compressed_buf.get()), &compressed_size);
		compressed_data = compressed_buf.get();
		break;
	}
	case CompressionCodec::ZSTD: {
		compressed_size = duckdb_zstd::ZSTD_compressBound(temp_writer.GetPosition());
		compressed_buf = BufferAllocator::Get(writer.GetContext()).Allocate(compressed_size);
		compressed_size = duckdb_zstd::ZSTD_compress((void *)compressed_buf.get(), compressed_size,
		                                             (const void *)temp_writer.GetData(), temp_writer.GetPosition(),
		                                             UnsafeNumericCast<int32_t>(writer.CompressionLevel()));
		compressed_data = compressed_buf.get();
		break;
	}
	case CompressionCodec::BROTLI: {
		compressed_size = duckdb_brotli::BrotliEncoderMaxCompressedSize(temp_writer.GetPosition());
		compressed_buf = BufferAllocator::Get(writer.GetContext()).Allocate(compressed_size);
		duckdb_brotli::BrotliEncoderCompress(BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
		                                     temp_writer.GetPosition(), temp_writer.GetData(), &compressed_size,
		                                     compressed_buf.get());
		compressed_data = compressed_buf.get();
		break;
	}
	default:
		throw InternalException("Unsupported codec for Parquet Writer");
	}

	if (compressed_size > idx_t(NumericLimits<int32_t>::Maximum())) {
		throw InternalException("Parquet writer: %d compressed page size out of range for type integer",
		                        temp_writer.GetPosition());
	}
}

void ColumnWriter::HandleRepeatLevels(ColumnWriterState &state, ColumnWriterState *parent, idx_t count) const {
	if (!parent) {
		// no repeat levels without a parent node
		return;
	}
	if (state.repetition_levels.size() >= parent->repetition_levels.size()) {
		return;
	}
	auto repetition_offset = NumericCast<ptrdiff_t>(state.repetition_levels.size());
	state.repetition_levels.insert(state.repetition_levels.end(), parent->repetition_levels.begin() + repetition_offset,
	                               parent->repetition_levels.end());
}

void ColumnWriter::HandleDefineLevels(ColumnWriterState &state, ColumnWriterState *parent, const ValidityMask &validity,
                                      const idx_t count, const uint16_t define_value, const uint16_t null_value) const {
	if (parent) {
		// parent node: inherit definition level from the parent
		idx_t vector_index = 0;
		while (state.definition_levels.size() < parent->definition_levels.size()) {
			idx_t current_index = state.definition_levels.size();
			if (parent->definition_levels[current_index] != PARQUET_DEFINE_VALID) {
				//! Inherit nulls from parent
				state.definition_levels.push_back(parent->definition_levels[current_index]);
				state.parent_null_count++;
			} else if (validity.RowIsValid(vector_index)) {
				//! Produce a non-null define
				state.definition_levels.push_back(define_value);
			} else {
				//! Produce a null define
				if (!can_have_nulls) {
					throw IOException("Parquet writer: map key column is not allowed to contain NULL values");
				}
				state.null_count++;
				state.definition_levels.push_back(null_value);
			}
			D_ASSERT(parent->is_empty.empty() || current_index < parent->is_empty.size());
			if (parent->is_empty.empty() || !parent->is_empty[current_index]) {
				vector_index++;
			}
		}
		return;
	}

	// no parent: set definition levels only from this validity mask
	if (validity.CannotHaveNull()) {
		state.definition_levels.insert(state.definition_levels.end(), count, define_value);
	} else {
		for (idx_t i = 0; i < count; i++) {
			const auto is_null = !validity.RowIsValid(i);
			state.definition_levels.emplace_back(is_null ? null_value : define_value);
			state.null_count += is_null;
		}
	}
	if (!can_have_nulls && state.null_count != 0) {
		throw IOException("Parquet writer: map key column is not allowed to contain NULL values");
	}
}

//===--------------------------------------------------------------------===//
// Create Column Writer
//===--------------------------------------------------------------------===//

//! Creates the writer of a column without children
static unique_ptr<ColumnWriter> CreateLeafWriter(ParquetWriter &writer, ParquetColumnSchema schema,
                                                 vector<Identifier> path_in_schema, const LogicalType &type) {
	const bool parquet_write_timestamp_as_int96 = writer.WriteTimestampAsInt96();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return make_uniq<BooleanColumnWriter>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::TINYINT:
		return make_uniq<StandardColumnWriter<int8_t, int32_t>>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::SMALLINT:
		return make_uniq<StandardColumnWriter<int16_t, int32_t>>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::DATE:
		return make_uniq<StandardColumnWriter<int32_t, int32_t>>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
		return make_uniq<StandardColumnWriter<int64_t, int64_t>>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		if (parquet_write_timestamp_as_int96) {
			return make_uniq<StandardColumnWriter<int64_t, Int96, ParquetTimestampInt96Operator>>(
			    writer, std::move(schema), path_in_schema);
		}
		return make_uniq<StandardColumnWriter<int64_t, int64_t>>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::TIMESTAMP_MS:
		if (parquet_write_timestamp_as_int96) {
			return make_uniq<StandardColumnWriter<int64_t, Int96, ParquetTimestampMSInt96Operator>>(
			    writer, std::move(schema), path_in_schema);
		}
		return make_uniq<StandardColumnWriter<int64_t, int64_t>>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::TIME_TZ:
		return make_uniq<StandardColumnWriter<dtime_tz_t, int64_t, ParquetTimeTZOperator>>(writer, std::move(schema),
		                                                                                   path_in_schema);
	case LogicalTypeId::HUGEINT:
		return make_uniq<
		    StandardColumnWriter<hugeint_t, ParquetHugeintDecimalTargetType, ParquetHugeintDecimalOperator>>(
		    writer, std::move(schema), path_in_schema);
	case LogicalTypeId::UHUGEINT:
		return make_uniq<
		    StandardColumnWriter<uhugeint_t, ParquetHugeintDecimalTargetType, ParquetHugeintDecimalOperator>>(
		    writer, std::move(schema), path_in_schema);
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		if (parquet_write_timestamp_as_int96) {
			return make_uniq<StandardColumnWriter<int64_t, Int96, ParquetTimestampNSInt96Operator>>(
			    writer, std::move(schema), path_in_schema);
		}
		return make_uniq<StandardColumnWriter<int64_t, int64_t, ParquetTimestampNSOperator>>(writer, std::move(schema),
		                                                                                     path_in_schema);
	case LogicalTypeId::TIMESTAMP_SEC:
		if (parquet_write_timestamp_as_int96) {
			return make_uniq<StandardColumnWriter<int64_t, Int96, ParquetTimestampSInt96Operator>>(
			    writer, std::move(schema), path_in_schema);
		}
		return make_uniq<StandardColumnWriter<int64_t, int64_t, ParquetTimestampSOperator>>(writer, std::move(schema),
		                                                                                    path_in_schema);
	case LogicalTypeId::UTINYINT:
		return make_uniq<StandardColumnWriter<uint8_t, int32_t>>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::USMALLINT:
		return make_uniq<StandardColumnWriter<uint16_t, int32_t>>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::UINTEGER:
		return make_uniq<StandardColumnWriter<uint32_t, uint32_t>>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::UBIGINT:
		return make_uniq<StandardColumnWriter<uint64_t, uint64_t>>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::FLOAT:
		return make_uniq<StandardColumnWriter<float_na_equal, float, FloatingPointOperator>>(writer, std::move(schema),
		                                                                                     path_in_schema);
	case LogicalTypeId::DOUBLE:
		return make_uniq<StandardColumnWriter<double_na_equal, double, FloatingPointOperator>>(
		    writer, std::move(schema), path_in_schema);
	case LogicalTypeId::DECIMAL:
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return make_uniq<StandardColumnWriter<int16_t, int32_t>>(writer, std::move(schema), path_in_schema);
		case PhysicalType::INT32:
			return make_uniq<StandardColumnWriter<int32_t, int32_t>>(writer, std::move(schema), path_in_schema);
		case PhysicalType::INT64:
			return make_uniq<StandardColumnWriter<int64_t, int64_t>>(writer, std::move(schema), path_in_schema);
		case PhysicalType::VARCHAR:
			// wide decimal stored as a BIGNUM blob
			return make_uniq<WideDecimalColumnWriter>(writer, std::move(schema), path_in_schema);
		default:
			return make_uniq<FixedDecimalColumnWriter>(writer, std::move(schema), path_in_schema);
		}
	case LogicalTypeId::BLOB:
		return make_uniq<StandardColumnWriter<string_t, string_t, ParquetBlobOperator>>(writer, std::move(schema),
		                                                                                path_in_schema);
	case LogicalTypeId::GEOMETRY:
		return make_uniq<StandardColumnWriter<string_t, string_t, ParquetGeometryOperator>>(writer, std::move(schema),
		                                                                                    path_in_schema);
	case LogicalTypeId::GEOGRAPHY:
		return make_uniq<StandardColumnWriter<string_t, string_t, ParquetGeographyOperator>>(writer, std::move(schema),
		                                                                                     path_in_schema);
	case LogicalTypeId::VARCHAR:
		return make_uniq<StandardColumnWriter<string_t, string_t, ParquetStringOperator>>(writer, std::move(schema),
		                                                                                  path_in_schema);
	case LogicalTypeId::UUID:
		return make_uniq<StandardColumnWriter<hugeint_t, ParquetUUIDTargetType, ParquetUUIDOperator>>(
		    writer, std::move(schema), path_in_schema);
	case LogicalTypeId::INTERVAL:
		return make_uniq<
		    StandardColumnWriter<interval_no_normalize, ParquetIntervalTargetType, ParquetIntervalOperator>>(
		    writer, std::move(schema), path_in_schema);
	case LogicalTypeId::ENUM:
		return make_uniq<EnumColumnWriter>(writer, std::move(schema), path_in_schema);
	case LogicalTypeId::SQLNULL:
		// All values are NULL - use INT32 as physical type (values are never read, only definition levels matter)
		return make_uniq<StandardColumnWriter<int32_t, int32_t>>(writer, std::move(schema), path_in_schema);
	default:
		throw InternalException("Unsupported type \"%s\" in Parquet writer", type.ToString());
	}
}

enum class CreateWriterFrameType { VARIANT, STRUCT, LIST, MAP };

//! A nested column whose child writers are being created. The open columns are kept on an explicit stack, so that
//! deeply nested types cannot overflow the call stack
struct CreateWriterFrame {
	CreateWriterFrame(CreateWriterFrameType frame_type_p, vector<Identifier> path_in_schema_p,
	                  const LogicalType &type_p, const Identifier &name_p, bool allow_geometry_p, idx_t max_repeat_p,
	                  idx_t max_define_p, FieldRepetitionType::type null_type_p)
	    : frame_type(frame_type_p), path_in_schema(std::move(path_in_schema_p)), type(type_p), name(name_p),
	      allow_geometry(allow_geometry_p), max_repeat(max_repeat_p), max_define(max_define_p), null_type(null_type_p) {
	}

	idx_t ChildCount() const {
		return frame_type == CreateWriterFrameType::LIST ? 1 : child_types.size();
	}

	CreateWriterFrameType frame_type;
	//! The path of the column, and of the group that holds its children for a LIST or a MAP
	vector<Identifier> path_in_schema;
	LogicalType type;
	Identifier name;
	bool allow_geometry;
	idx_t max_repeat;
	idx_t max_define;
	FieldRepetitionType::type null_type;
	optional_ptr<const FieldID> field_id;
	optional_ptr<const ChildFieldIDs> child_field_ids;
	optional_ptr<const ShreddingType> shredding_type;
	//! Whether a VARIANT has a 'typed_value'
	bool is_shredded = false;
	//! The children of a VARIANT, a STRUCT or a MAP
	child_list_t<LogicalType> child_types;
	//! The writers of the children that are done
	vector<unique_ptr<ColumnWriter>> child_writers;
};

//! Creates the writer of a column without children into 'result', or opens the frame of a nested column and returns
//! true
static bool OpenWriterFrame(ParquetWriter &writer, vector<Identifier> path_in_schema, const LogicalType &type,
                            const Identifier &name, bool allow_geometry, optional_ptr<const ChildFieldIDs> field_ids,
                            optional_ptr<const ShreddingType> shredding_types, idx_t max_repeat, idx_t max_define,
                            bool can_have_nulls, vector<CreateWriterFrame> &frames, unique_ptr<ColumnWriter> &result) {
	path_in_schema.emplace_back(name);

	if (!can_have_nulls) {
		max_define--;
	}
	auto null_type = can_have_nulls ? FieldRepetitionType::OPTIONAL : FieldRepetitionType::REQUIRED;

	optional_ptr<const FieldID> field_id;
	optional_ptr<const ChildFieldIDs> child_field_ids;
	optional_ptr<const ShreddingType> shredding_type;
	if (field_ids) {
		auto field_id_it = field_ids->ids->find(name);
		if (field_id_it != field_ids->ids->end()) {
			field_id = &field_id_it->second;
			child_field_ids = &field_id->child_field_ids;
		}
	}
	if (shredding_types) {
		shredding_type = shredding_types->GetChild(name);
	}

	CreateWriterFrameType frame_type;
	if (type.id() == LogicalTypeId::VARIANT) {
		frame_type = CreateWriterFrameType::VARIANT;
	} else if (StructType::IsStruct(type.id()) || type.id() == LogicalTypeId::UNION) {
		if (StructType::IsStruct(type.id()) && StructType::GetChildTypes(type).empty()) {
			throw InvalidInputException("Empty STRUCT columns are not supported in the Parquet format");
		}
		frame_type = CreateWriterFrameType::STRUCT;
	} else if (type.id() == LogicalTypeId::LIST || type.id() == LogicalTypeId::ARRAY) {
		frame_type = CreateWriterFrameType::LIST;
	} else if (type.id() == LogicalTypeId::MAP) {
		frame_type = CreateWriterFrameType::MAP;
	} else {
		auto schema =
		    ParquetColumnSchema::FromLogicalType(name, type, max_define, max_repeat, 0, null_type, allow_geometry);
		if (field_id && field_id->set) {
			schema.field_id = field_id->field_id;
		}
		result = CreateLeafWriter(writer, std::move(schema), std::move(path_in_schema), type);
		return false;
	}

	CreateWriterFrame frame(frame_type, std::move(path_in_schema), type, name, allow_geometry, max_repeat, max_define,
	                        null_type);
	frame.field_id = field_id;
	frame.child_field_ids = child_field_ids;
	frame.shredding_type = shredding_type;
	switch (frame_type) {
	case CreateWriterFrameType::VARIANT: {
		frame.is_shredded = shredding_type && shredding_type->type.id() != LogicalTypeId::SQLNULL &&
		                    shredding_type->type.id() != LogicalTypeId::ANY;

		//! Build the child types for the Parquet VARIANT
		frame.child_types.emplace_back("metadata", LogicalType::BLOB);
		frame.child_types.emplace_back("value", LogicalType::BLOB);
		if (frame.is_shredded) {
			auto &typed_value_type = shredding_type->type;
			frame.child_types.emplace_back("typed_value",
			                               VariantColumnWriter::TransformTypedValueRecursive(typed_value_type));
		}
		break;
	}
	case CreateWriterFrameType::STRUCT:
		// TUPLE children have empty names - NamedChildren synthesizes positional ones for a valid Parquet schema
		frame.child_types = TupleType::NamedChildren(type);
		break;
	case CreateWriterFrameType::LIST:
		frame.path_in_schema.push_back("list");
		break;
	case CreateWriterFrameType::MAP:
		frame.path_in_schema.push_back("key_value");
		frame.child_types = LogicalType::GetNamedChildTypes(type);
		break;
	}
	frame.child_writers.reserve(frame.ChildCount());
	frames.push_back(std::move(frame));
	return true;
}

//! Creates the writer of the next child of a column into 'result', or opens the frame of that child and returns true
static bool OpenNextChildWriter(ParquetWriter &writer, vector<CreateWriterFrame> &frames,
                                unique_ptr<ColumnWriter> &result) {
	auto &frame = frames.back();
	auto child_idx = frame.child_writers.size();
	// the frame is not used after the child opened a frame of its own, which may move it
	switch (frame.frame_type) {
	case CreateWriterFrameType::VARIANT: {
		auto &child_name = frame.child_types[child_idx].first;
		auto &child_type = frame.child_types[child_idx].second;
		optional_ptr<const ShreddingType> child_shredding;
		bool is_optional;
		if (child_name == "metadata") {
			is_optional = false;
		} else if (child_name == "value") {
			//! When shredding the variant, the 'value' becomes optional
			is_optional = frame.is_shredded;
		} else {
			D_ASSERT(child_name == "typed_value");
			//! Only set shredding for the 'typed_value', just in case there's a child by the name of 'metadata' or
			//! 'value'
			child_shredding = frame.shredding_type;
			is_optional = true;
		}
		return OpenWriterFrame(writer, frame.path_in_schema, child_type, child_name, frame.allow_geometry,
		                       frame.child_field_ids, child_shredding, frame.max_repeat, frame.max_define + 1,
		                       is_optional, frames, result);
	}
	case CreateWriterFrameType::STRUCT: {
		auto &child_name = frame.child_types[child_idx].first;
		auto &child_type = frame.child_types[child_idx].second;
		return OpenWriterFrame(writer, frame.path_in_schema, child_type, child_name, frame.allow_geometry,
		                       frame.child_field_ids, frame.shredding_type, frame.max_repeat, frame.max_define + 1,
		                       true, frames, result);
	}
	case CreateWriterFrameType::LIST: {
		auto is_list = frame.type.id() == LogicalTypeId::LIST;
		auto &child_type = is_list ? ListType::GetChildType(frame.type) : ArrayType::GetChildType(frame.type);
		return OpenWriterFrame(writer, frame.path_in_schema, child_type, "element", frame.allow_geometry,
		                       frame.child_field_ids, frame.shredding_type, frame.max_repeat + 1, frame.max_define + 2,
		                       true, frames, result);
	}
	case CreateWriterFrameType::MAP: {
		// key needs to be marked as REQUIRED
		bool is_key = child_idx == 0;
		auto &child_name = frame.child_types[child_idx].first;
		auto &child_type = frame.child_types[child_idx].second;
		return OpenWriterFrame(writer, frame.path_in_schema, child_type, child_name, frame.allow_geometry,
		                       frame.child_field_ids, frame.shredding_type, frame.max_repeat + 1, frame.max_define + 2,
		                       !is_key, frames, result);
	}
	default:
		throw InternalException("Unsupported frame type in the Parquet writer");
	}
}

//! Creates the writer of a nested column whose children all have their writer
static unique_ptr<ColumnWriter> FinishWriterFrame(ParquetWriter &writer, CreateWriterFrame &frame) {
	auto &type = frame.type;
	auto &path_in_schema = frame.path_in_schema;
	auto column = ParquetColumnSchema::FromLogicalType(frame.name, type, frame.max_define, frame.max_repeat, 0,
	                                                   frame.null_type, frame.allow_geometry);
	if (frame.field_id && frame.field_id->set) {
		column.field_id = frame.field_id->field_id;
	}
	switch (frame.frame_type) {
	case CreateWriterFrameType::VARIANT:
		return make_uniq<VariantColumnWriter>(writer, std::move(column), path_in_schema, std::move(frame.child_writers),
		                                      !frame.shredding_type);
	case CreateWriterFrameType::STRUCT:
		return make_uniq<StructColumnWriter>(writer, std::move(column), path_in_schema, std::move(frame.child_writers));
	case CreateWriterFrameType::LIST:
		if (type.id() == LogicalTypeId::LIST) {
			return make_uniq<ListColumnWriter>(writer, std::move(column), path_in_schema,
			                                   std::move(frame.child_writers[0]));
		}
		return make_uniq<ArrayColumnWriter>(writer, std::move(column), path_in_schema,
		                                    std::move(frame.child_writers[0]));
	case CreateWriterFrameType::MAP: {
		auto key_value_type = LogicalType::STRUCT(frame.child_types);
		auto key_value_schema = ParquetColumnSchema::FromLogicalType(
		    "key_value", key_value_type, frame.max_define + 1, frame.max_repeat + 1, 0, FieldRepetitionType::REPEATED,
		    frame.allow_geometry);
		auto struct_writer = make_uniq<StructColumnWriter>(writer, std::move(key_value_schema), path_in_schema,
		                                                   std::move(frame.child_writers));
		return make_uniq<ListColumnWriter>(writer, std::move(column), path_in_schema, std::move(struct_writer));
	}
	default:
		throw InternalException("Unsupported frame type in the Parquet writer");
	}
}

unique_ptr<ColumnWriter> ColumnWriter::CreateWriterRecursive(ClientContext &context, ParquetWriter &writer,
                                                             vector<Identifier> path_in_schema, const LogicalType &type,
                                                             const Identifier &name, bool allow_geometry,
                                                             optional_ptr<const ChildFieldIDs> field_ids,
                                                             optional_ptr<const ShreddingType> shredding_types,
                                                             idx_t max_repeat, idx_t max_define, bool can_have_nulls) {
	vector<CreateWriterFrame> frames;
	unique_ptr<ColumnWriter> root;
	if (!OpenWriterFrame(writer, std::move(path_in_schema), type, name, allow_geometry, field_ids, shredding_types,
	                     max_repeat, max_define, can_have_nulls, frames, root)) {
		return root;
	}
	while (true) {
		if (frames.back().child_writers.size() < frames.back().ChildCount()) {
			unique_ptr<ColumnWriter> child;
			if (!OpenNextChildWriter(writer, frames, child)) {
				frames.back().child_writers.push_back(std::move(child));
			}
			continue;
		}
		auto column_writer = FinishWriterFrame(writer, frames.back());
		frames.pop_back();
		if (frames.empty()) {
			return column_writer;
		}
		frames.back().child_writers.push_back(std::move(column_writer));
	}
}

template <>
struct NumericLimits<float_na_equal> {
	static constexpr float Minimum() {
		return NumericLimits<float>::Minimum();
	};
	static constexpr float Maximum() {
		return NumericLimits<float>::Maximum();
	};
	static constexpr bool IsSigned() {
		return std::is_signed<float>::value;
	}
	static constexpr bool IsIntegral() {
		return std::is_integral<float>::value;
	}
};

template <>
struct NumericLimits<double_na_equal> {
	static constexpr double Minimum() {
		return NumericLimits<double>::Minimum();
	};
	static constexpr double Maximum() {
		return NumericLimits<double>::Maximum();
	};
	static constexpr bool IsSigned() {
		return std::is_signed<double>::value;
	}
	static constexpr bool IsIntegral() {
		return std::is_integral<double>::value;
	}
};

template <>
hash_t Hash(ParquetIntervalTargetType val) {
	return Hash(const_char_ptr_cast(val.bytes), ParquetIntervalTargetType::PARQUET_INTERVAL_SIZE);
}

template <>
hash_t Hash(ParquetUUIDTargetType val) {
	return Hash(const_char_ptr_cast(val.bytes), ParquetUUIDTargetType::PARQUET_UUID_SIZE);
}

template <>
hash_t Hash(ParquetHugeintDecimalTargetType val) {
	return Hash(const_char_ptr_cast(val.bytes), ParquetHugeintDecimalTargetType::PARQUET_SIZE);
}

template <>
hash_t Hash(float_na_equal val) {
	if (std::isnan(val.val)) {
		return Hash<float>(std::numeric_limits<float>::quiet_NaN());
	}
	return Hash<float>(val.val);
}

template <>
hash_t Hash(double_na_equal val) {
	if (std::isnan(val.val)) {
		return Hash<double>(std::numeric_limits<double>::quiet_NaN());
	}
	return Hash<double>(val.val);
}

} // namespace duckdb
