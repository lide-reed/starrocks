// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include <memory>

#include "column/array_column.h"
#include "column/binary_column.h"
#include "column/chunk.h"
#include "column/column_helper.h"
#include "column/const_column.h"
#include "column/decimalv3_column.h"
#include "column/field.h"
#include "column/nullable_column.h"
#include "column/object_column.h"
#include "column/schema.h"
#include "column/vectorized_fwd.h"
#include "storage/schema.h"

namespace starrocks {

class Status;
class TabletColumn;
class TabletSchema;

namespace vectorized {

class Chunk;
class Field;
class Column;
class Schema;

class ChunkHelper {
public:
    static vectorized::Field convert_field(ColumnId id, const TabletColumn& c);

    static vectorized::Schema convert_schema(const starrocks::TabletSchema& schema);

    // Convert starrocks::TabletColumn to vectorized::Field. This function will generate format
    // V2 type: DATE_V2, TIMESTAMP, DECIMAL_V2
    static vectorized::Field convert_field_to_format_v2(ColumnId id, const TabletColumn& c);

    // Convert TabletSchema to vectorized::Schema with changing format v1 type to format v2 type.
    static vectorized::Schema convert_schema_to_format_v2(const starrocks::TabletSchema& schema);

    // Convert TabletSchema to vectorized::Schema with changing format v1 type to format v2 type.
    static vectorized::Schema convert_schema_to_format_v2(const starrocks::TabletSchema& schema,
                                                          const std::vector<ColumnId>& cids);

    static ColumnId max_column_id(const vectorized::Schema& schema);

    // Create an empty chunk according to the |schema| and reserve it of size |n|.
    static std::shared_ptr<Chunk> new_chunk(const vectorized::Schema& schema, size_t n);

    // Create an empty chunk according to the |tuple_desc| and reserve it of size |n|.
    static std::shared_ptr<Chunk> new_chunk(const TupleDescriptor& tuple_desc, size_t n);

    // Create an empty chunk according to the |slots| and reserve it of size |n|.
    static std::shared_ptr<Chunk> new_chunk(const std::vector<SlotDescriptor*>& slots, size_t n);

    static Chunk* new_chunk_pooled(const vectorized::Schema& schema, size_t n, bool force = true);

    // Create a vectorized column from field .
    // REQUIRE: |type| must be scalar type.
    static std::shared_ptr<Column> column_from_field_type(FieldType type, bool nullable);

    // Create a vectorized column from field.
    static std::shared_ptr<Column> column_from_field(const Field& field);

    // FieldType data size in memory
    static size_t approximate_sizeof_type(FieldType type);

    // Get char column indexes
    static std::vector<size_t> get_char_field_indexes(const vectorized::Schema& schema);

    // Padding char columns
    static void padding_char_columns(const std::vector<size_t>& char_column_indexes, const vectorized::Schema& schema,
                                     const starrocks::TabletSchema& tschema, vectorized::Chunk* chunk);
};

inline ChunkPtr ChunkHelper::new_chunk(const vectorized::Schema& schema, size_t n) {
    size_t fields = schema.num_fields();
    Columns columns;
    columns.reserve(fields);
    for (size_t i = 0; i < fields; i++) {
        const vectorized::FieldPtr& f = schema.field(i);
        columns.emplace_back(column_from_field(*f));
        columns.back()->reserve(n);
    }
    return std::make_shared<Chunk>(std::move(columns), std::make_shared<vectorized::Schema>(schema));
}

inline std::shared_ptr<Chunk> ChunkHelper::new_chunk(const TupleDescriptor& tuple_desc, size_t n) {
    return new_chunk(tuple_desc.slots(), n);
}

inline std::shared_ptr<Chunk> ChunkHelper::new_chunk(const std::vector<SlotDescriptor*>& slots, size_t n) {
    auto chunk = std::make_shared<Chunk>();
    for (const auto slot : slots) {
        ColumnPtr column = ColumnHelper::create_column(slot->type(), slot->is_nullable());
        column->reserve(n);
        chunk->append_column(column, slot->id());
    }
    return chunk;
}

inline ColumnPtr ChunkHelper::column_from_field_type(FieldType type, bool nullable) {
    auto NullableIfNeed = [&](ColumnPtr col) -> ColumnPtr {
        return nullable ? NullableColumn::create(std::move(col), NullColumn::create()) : col;
    };

    switch (type) {
    case OLAP_FIELD_TYPE_DECIMAL:
        return NullableIfNeed(FixedLengthColumn<decimal12_t>::create());
    case OLAP_FIELD_TYPE_DECIMAL_V2:
        return NullableIfNeed(DecimalColumn::create());
    case OLAP_FIELD_TYPE_HLL:
        return NullableIfNeed(HyperLogLogColumn::create());
    case OLAP_FIELD_TYPE_OBJECT:
        return NullableIfNeed(BitmapColumn::create());
    case OLAP_FIELD_TYPE_PERCENTILE:
        return NullableIfNeed(PercentileColumn::create());
    case OLAP_FIELD_TYPE_CHAR:
    case OLAP_FIELD_TYPE_VARCHAR:
        return NullableIfNeed(BinaryColumn::create());
    case OLAP_FIELD_TYPE_BOOL:
        return NullableIfNeed(FixedLengthColumn<uint8_t>::create());
    case OLAP_FIELD_TYPE_TINYINT:
        return NullableIfNeed(FixedLengthColumn<int8_t>::create());
    case OLAP_FIELD_TYPE_SMALLINT:
        return NullableIfNeed(FixedLengthColumn<int16_t>::create());
    case OLAP_FIELD_TYPE_INT:
        return NullableIfNeed(FixedLengthColumn<int32_t>::create());
    case OLAP_FIELD_TYPE_UNSIGNED_INT:
        return NullableIfNeed(FixedLengthColumn<uint32_t>::create());
    case OLAP_FIELD_TYPE_BIGINT:
        return NullableIfNeed(FixedLengthColumn<int64_t>::create());
    case OLAP_FIELD_TYPE_UNSIGNED_BIGINT:
        return NullableIfNeed(FixedLengthColumn<uint64_t>::create());
    case OLAP_FIELD_TYPE_LARGEINT:
        return NullableIfNeed(FixedLengthColumn<int128_t>::create());
    case OLAP_FIELD_TYPE_FLOAT:
        return NullableIfNeed(FixedLengthColumn<float>::create());
    case OLAP_FIELD_TYPE_DOUBLE:
        return NullableIfNeed(FixedLengthColumn<double>::create());
    case OLAP_FIELD_TYPE_DATE:
        return NullableIfNeed(FixedLengthColumn<uint24_t>::create());
    case OLAP_FIELD_TYPE_DATE_V2:
        return NullableIfNeed(DateColumn::create());
    case OLAP_FIELD_TYPE_DATETIME:
        return NullableIfNeed(FixedLengthColumn<int64_t>::create());
    case OLAP_FIELD_TYPE_TIMESTAMP:
        return NullableIfNeed(TimestampColumn::create());
    case OLAP_FIELD_TYPE_DECIMAL32:
    case OLAP_FIELD_TYPE_DECIMAL64:
    case OLAP_FIELD_TYPE_DECIMAL128:
    case OLAP_FIELD_TYPE_ARRAY:
    case OLAP_FIELD_TYPE_UNSIGNED_TINYINT:
    case OLAP_FIELD_TYPE_UNSIGNED_SMALLINT:
    case OLAP_FIELD_TYPE_DISCRETE_DOUBLE:
    case OLAP_FIELD_TYPE_STRUCT:
    case OLAP_FIELD_TYPE_MAP:
    case OLAP_FIELD_TYPE_UNKNOWN:
    case OLAP_FIELD_TYPE_NONE:
    case OLAP_FIELD_TYPE_MAX_VALUE:
        break;
    }
    return nullptr;
}

inline ColumnPtr ChunkHelper::column_from_field(const Field& field) {
    auto NullableIfNeed = [&](ColumnPtr col) -> ColumnPtr {
        return field.is_nullable() ? NullableColumn::create(std::move(col), NullColumn::create()) : col;
    };

    auto type = field.type()->type();
    switch (type) {
    case OLAP_FIELD_TYPE_DECIMAL32:
        return NullableIfNeed(Decimal32Column::create(field.type()->precision(), field.type()->scale()));
    case OLAP_FIELD_TYPE_DECIMAL64:
        return NullableIfNeed(Decimal64Column::create(field.type()->precision(), field.type()->scale()));
    case OLAP_FIELD_TYPE_DECIMAL128:
        return NullableIfNeed(Decimal128Column::create(field.type()->precision(), field.type()->scale()));
    case OLAP_FIELD_TYPE_ARRAY: {
        return NullableIfNeed(ArrayColumn::create(column_from_field(field.sub_field(0)), UInt32Column::create()));
    }
    default:
        return NullableIfNeed(column_from_field_type(type, false));
    }
}

} // namespace vectorized
} // namespace starrocks
