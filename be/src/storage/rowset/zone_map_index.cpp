// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/rowset/segment_v2/zone_map_index.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "storage/rowset/zone_map_index.h"

#include "runtime/mem_pool.h"
#include "runtime/mem_tracker.h"
#include "storage/column_block.h"
#include "storage/fs/block_manager.h"
#include "storage/olap_define.h"
#include "storage/rowset/encoding_info.h"
#include "storage/rowset/indexed_column_reader.h"
#include "storage/rowset/indexed_column_writer.h"
#include "storage/types.h"
#include "util/unaligned_access.h"

namespace starrocks {

struct ZoneMap {
    // min value of zone
    char* min_value = nullptr;
    // max value of zone
    char* max_value = nullptr;

    // if both has_null and has_not_null is false, means no rows.
    // if has_null is true and has_not_null is false, means all rows is null.
    // if has_null is false and has_not_null is true, means all rows is not null.
    // if has_null is true and has_not_null is true, means some rows is null and others are not.
    // has_null means whether zone has null value
    bool has_null = false;
    // has_not_null means whether zone has none-null value
    bool has_not_null = false;

    void to_proto(ZoneMapPB* dst, Field* field) const {
        dst->set_min(field->to_zone_map_string(min_value));
        dst->set_max(field->to_zone_map_string(max_value));
        dst->set_has_null(has_null);
        dst->set_has_not_null(has_not_null);
    }
};

template <FieldType type>
class ZoneMapIndexWriterImpl final : public ZoneMapIndexWriter {
    using CppType = typename TypeTraits<type>::CppType;

public:
    explicit ZoneMapIndexWriterImpl(starrocks::Field* field);

    void add_values(const void* values, size_t count) override;

    void add_nulls(uint32_t count) override { _page_zone_map.has_null |= count > 0; }

    // mark the end of one data page so that we can finalize the corresponding zone map
    Status flush() override;

    Status finish(fs::WritableBlock* wblock, ColumnIndexMetaPB* index_meta) override;

    uint64_t size() const override { return _estimated_size; }

private:
    void _reset_zone_map(ZoneMap* zone_map) {
        // we should allocate max varchar length and set to max for min value
        _field->set_to_max(zone_map->min_value);
        _field->set_to_min(zone_map->max_value);
        zone_map->has_null = false;
        zone_map->has_not_null = false;
    }

    Field* _field;
    // memory will be managed by MemPool
    ZoneMap _page_zone_map;
    ZoneMap _segment_zone_map;
    // TODO(zc): we should replace this memory pool later, we only allocate min/max
    // for field. But MemPool allocate 4KB least, it will a waste for most cases.
    MemPool _pool;

    // serialized ZoneMapPB for each data page
    std::vector<std::string> _values;
    uint64_t _estimated_size = 0;
};

template <FieldType type>
ZoneMapIndexWriterImpl<type>::ZoneMapIndexWriterImpl(Field* field) : _field(field) {
    _page_zone_map.min_value = _field->allocate_value(&_pool);
    _page_zone_map.max_value = _field->allocate_value(&_pool);
    _reset_zone_map(&_page_zone_map);
    _segment_zone_map.min_value = _field->allocate_value(&_pool);
    _segment_zone_map.max_value = _field->allocate_value(&_pool);
    _reset_zone_map(&_segment_zone_map);
}

template <FieldType type>
void ZoneMapIndexWriterImpl<type>::add_values(const void* values, size_t count) {
    if (count > 0) {
        _page_zone_map.has_not_null = true;
        const CppType* vals = reinterpret_cast<const CppType*>(values);
        auto [pmin, pmax] = std::minmax_element(vals, vals + count);
        if (unaligned_load<CppType>(pmin) < unaligned_load<CppType>(_page_zone_map.min_value)) {
            _field->type_info()->direct_copy(_page_zone_map.min_value, pmin, nullptr);
        }
        if (unaligned_load<CppType>(pmax) > unaligned_load<CppType>(_page_zone_map.max_value)) {
            _field->type_info()->direct_copy(_page_zone_map.max_value, pmax, nullptr);
        }
    }
}

template <FieldType type>
Status ZoneMapIndexWriterImpl<type>::flush() {
    // Update segment zone map.
    if (_field->compare(_segment_zone_map.min_value, _page_zone_map.min_value) > 0) {
        _field->type_info()->direct_copy(_segment_zone_map.min_value, _page_zone_map.min_value, nullptr);
    }
    if (_field->compare(_segment_zone_map.max_value, _page_zone_map.max_value) < 0) {
        _field->type_info()->direct_copy(_segment_zone_map.max_value, _page_zone_map.max_value, nullptr);
    }
    if (_page_zone_map.has_null) {
        _segment_zone_map.has_null = true;
    }
    if (_page_zone_map.has_not_null) {
        _segment_zone_map.has_not_null = true;
    }

    ZoneMapPB zone_map_pb;
    _page_zone_map.to_proto(&zone_map_pb, _field);
    _reset_zone_map(&_page_zone_map);

    std::string serialized_zone_map;
    bool ret = zone_map_pb.SerializeToString(&serialized_zone_map);
    if (!ret) {
        return Status::InternalError("serialize zone map failed");
    }
    _estimated_size += serialized_zone_map.size() + sizeof(uint32_t);
    _values.push_back(std::move(serialized_zone_map));
    return Status::OK();
}

std::unique_ptr<ZoneMapIndexWriter> ZoneMapIndexWriter::create(starrocks::Field* field) {
    switch (field->type()) {
    case OLAP_FIELD_TYPE_BOOL:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_BOOL>>(field);
    case OLAP_FIELD_TYPE_TINYINT:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_TINYINT>>(field);
    case OLAP_FIELD_TYPE_SMALLINT:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_SMALLINT>>(field);
    case OLAP_FIELD_TYPE_INT:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_INT>>(field);
    case OLAP_FIELD_TYPE_BIGINT:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_BIGINT>>(field);
    case OLAP_FIELD_TYPE_LARGEINT:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_LARGEINT>>(field);
    case OLAP_FIELD_TYPE_FLOAT:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_FLOAT>>(field);
    case OLAP_FIELD_TYPE_DOUBLE:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_DOUBLE>>(field);
    case OLAP_FIELD_TYPE_DECIMAL:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_DECIMAL>>(field);
    case OLAP_FIELD_TYPE_DECIMAL_V2:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_DECIMAL_V2>>(field);
    case OLAP_FIELD_TYPE_DECIMAL32:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_DECIMAL32>>(field);
    case OLAP_FIELD_TYPE_DECIMAL64:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_DECIMAL64>>(field);
    case OLAP_FIELD_TYPE_DECIMAL128:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_DECIMAL128>>(field);
    case OLAP_FIELD_TYPE_CHAR:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_CHAR>>(field);
    case OLAP_FIELD_TYPE_DATE:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_DATE>>(field);
    case OLAP_FIELD_TYPE_DATE_V2:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_DATE_V2>>(field);
    case OLAP_FIELD_TYPE_DATETIME:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_DATETIME>>(field);
    case OLAP_FIELD_TYPE_TIMESTAMP:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_TIMESTAMP>>(field);
    case OLAP_FIELD_TYPE_VARCHAR:
        return std::make_unique<ZoneMapIndexWriterImpl<OLAP_FIELD_TYPE_VARCHAR>>(field);
    case OLAP_FIELD_TYPE_STRUCT:
    case OLAP_FIELD_TYPE_ARRAY:
    case OLAP_FIELD_TYPE_MAP:
    case OLAP_FIELD_TYPE_UNKNOWN:
    case OLAP_FIELD_TYPE_NONE:
    case OLAP_FIELD_TYPE_HLL:
    case OLAP_FIELD_TYPE_OBJECT:
    case OLAP_FIELD_TYPE_UNSIGNED_TINYINT:
    case OLAP_FIELD_TYPE_UNSIGNED_SMALLINT:
    case OLAP_FIELD_TYPE_UNSIGNED_INT:
    case OLAP_FIELD_TYPE_UNSIGNED_BIGINT:
    case OLAP_FIELD_TYPE_DISCRETE_DOUBLE:
    case OLAP_FIELD_TYPE_PERCENTILE:
    case OLAP_FIELD_TYPE_MAX_VALUE:
        break;
    }
    return nullptr;
}

template <FieldType type>
Status ZoneMapIndexWriterImpl<type>::finish(fs::WritableBlock* wblock, ColumnIndexMetaPB* index_meta) {
    index_meta->set_type(ZONE_MAP_INDEX);
    ZoneMapIndexPB* meta = index_meta->mutable_zone_map_index();
    // store segment zone map
    _segment_zone_map.to_proto(meta->mutable_segment_zone_map(), _field);

    // write out zone map for each data pages
    TypeInfoPtr typeinfo = get_type_info(OLAP_FIELD_TYPE_OBJECT);
    IndexedColumnWriterOptions options;
    options.write_ordinal_index = true;
    options.write_value_index = false;
    options.encoding = EncodingInfo::get_default_encoding(OLAP_FIELD_TYPE_OBJECT, false);
    options.compression = NO_COMPRESSION; // currently not compressed

    IndexedColumnWriter writer(options, typeinfo, wblock);
    RETURN_IF_ERROR(writer.init());

    for (auto& value : _values) {
        Slice value_slice(value);
        RETURN_IF_ERROR(writer.add(&value_slice));
    }
    return writer.finish(meta->mutable_page_zone_maps());
}

Status ZoneMapIndexReader::load(fs::BlockManager* block_mgr, const std::string& filename,
                                const ZoneMapIndexPB* index_meta, bool use_page_cache, bool kept_in_memory) {
    IndexedColumnReader reader(block_mgr, filename, index_meta->page_zone_maps());
    RETURN_IF_ERROR(reader.load(use_page_cache, kept_in_memory));
    std::unique_ptr<IndexedColumnIterator> iter;
    RETURN_IF_ERROR(reader.new_iterator(&iter));

    MemPool pool;
    _page_zone_maps.resize(reader.num_values());

    // read and cache all page zone maps
    for (int i = 0; i < reader.num_values(); ++i) {
        size_t num_to_read = 1;
        std::unique_ptr<ColumnVectorBatch> cvb;
        RETURN_IF_ERROR(ColumnVectorBatch::create(num_to_read, false, reader.type_info(), nullptr, &cvb));
        ColumnBlock block(cvb.get(), &pool);
        ColumnBlockView column_block_view(&block);

        RETURN_IF_ERROR(iter->seek_to_ordinal(i));
        size_t num_read = num_to_read;
        RETURN_IF_ERROR(iter->next_batch(&num_read, &column_block_view));
        DCHECK(num_to_read == num_read);

        auto* value = reinterpret_cast<Slice*>(cvb->data());
        if (!_page_zone_maps[i].ParseFromArray(value->data, value->size)) {
            return Status::Corruption("Failed to parse zone map");
        }
        pool.clear();
    }
    return Status::OK();
}

} // namespace starrocks
