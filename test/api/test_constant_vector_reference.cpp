#include "catch.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"

using namespace duckdb;

// duckdblabs/bot-issue-tracker#638: a constant reference shares the heap of the string instead of copying it
TEST_CASE("A constant reference to a string keeps the heap that holds it", "[api]") {
	const string long_string(100000, 'x');
	auto make_source = [&]() {
		Vector source(LogicalType::VARCHAR, 4);
		auto data = FlatVector::GetDataMutable<string_t>(source);
		for (idx_t i = 0; i < 4; i++) {
			data[i] = StringVector::AddString(source, i == 2 ? long_string : string("short"));
		}
		FlatVector::SetSize(source, 4);
		return source;
	};

	// the constant shares the bytes of the string instead of copying them
	Vector reference(LogicalType::VARCHAR);
	{
		auto source = make_source();
		ConstantVector::Reference(reference, count_t(3000), source, 2, 4);
		REQUIRE(ConstantVector::GetData<string_t>(reference)->GetData() ==
		        FlatVector::GetData<string_t>(source)[2].GetData());
	}

	// the source goes away before the reference is read
	{
		auto source = make_source();
		ConstantVector::Reference(reference, count_t(3000), source, 2, 4);
	}
	REQUIRE(reference.GetVectorType() == VectorType::CONSTANT_VECTOR);
	REQUIRE(reference.GetValue(0) == Value(long_string));
	REQUIRE(reference.GetValue(2999) == Value(long_string));

	// the vector is the source itself
	auto vector = make_source();
	ConstantVector::Reference(vector, count_t(3000), vector, 2, 4);
	REQUIRE(vector.GetVectorType() == VectorType::CONSTANT_VECTOR);
	// the constant holds the heap of the string, which nothing else holds now
	REQUIRE(vector.GetBufferRef()->GetAuxiliaryData());
	REQUIRE(vector.GetValue(1) == Value(long_string));

	// a NULL gives a NULL constant
	auto with_null = make_source();
	FlatVector::SetNull(with_null, 1, true);
	ConstantVector::Reference(reference, count_t(10), with_null, 1, 4);
	REQUIRE(ConstantVector::IsNull(reference));
}

TEST_CASE("A constant reference to a string of a slice keeps it after the sliced chunk is reset", "[api]") {
	const string long_string(100000, 'x');
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::VARCHAR});
	auto fill = [&](const string &value) {
		auto data = FlatVector::GetDataMutable<string_t>(chunk.data[0]);
		for (idx_t i = 0; i < 4; i++) {
			data[i] = StringVector::AddString(chunk.data[0], i == 2 ? value : string("short"));
		}
		chunk.SetCardinality(4);
	};
	fill(long_string);

	Vector reference(LogicalType::VARCHAR);
	{
		// an offset slice holds the buffer of the chunk, whose heap holds the string
		Vector slice(LogicalType::VARCHAR);
		slice.Slice(chunk.data[0], 1, 4);
		ConstantVector::Reference(reference, count_t(4097), slice, 1, 3);
	}
	// a reset of the cached chunk releases its heap, and the chunk is filled again
	chunk.Reset();
	fill(string(long_string.size(), 'y'));
	REQUIRE(reference.GetValue(0) == Value(long_string));
	REQUIRE(reference.GetValue(4096) == Value(long_string));
}
