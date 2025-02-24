// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/test/olap/rowset/segment_v2/segment_test.cpp

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

#include "storage/rowset/segment.h"

#include <gtest/gtest.h>

#include <functional>
#include <iostream>

#include "column/datum_tuple.h"
#include "common/logging.h"
#include "env/env_memory.h"
#include "gutil/strings/substitute.h"
#include "runtime/mem_pool.h"
#include "runtime/mem_tracker.h"
#include "storage/fs/file_block_manager.h"
#include "storage/olap_common.h"
#include "storage/rowset/column_iterator.h"
#include "storage/rowset/column_reader.h"
#include "storage/rowset/segment_writer.h"
#include "storage/rowset/vectorized/segment_options.h"
#include "storage/tablet_schema.h"
#include "storage/tablet_schema_helper.h"
#include "storage/vectorized/chunk_helper.h"
#include "storage/vectorized/chunk_iterator.h"
#include "util/file_utils.h"

#define ASSERT_OK(expr)                                   \
    do {                                                  \
        Status _status = (expr);                          \
        ASSERT_TRUE(_status.ok()) << _status.to_string(); \
    } while (0)

namespace starrocks {

using std::string;
using std::shared_ptr;

using std::vector;

using ValueGenerator = std::function<vectorized::Datum(size_t rid, int cid, int block_id)>;

// 0,  1,  2,  3
// 10, 11, 12, 13
// 20, 21, 22, 23
static vectorized::Datum DefaultIntGenerator(size_t rid, int cid, int block_id) {
    return vectorized::Datum(static_cast<int32_t>(rid * 10 + cid));
}

class SegmentReaderWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        _env = new EnvMemory();
        _block_mgr = new fs::FileBlockManager(_env, fs::BlockManagerOptions());
        ASSERT_TRUE(_env->create_dir(kSegmentDir).ok());
        _page_cache_mem_tracker = std::make_unique<MemTracker>();
        _tablet_meta_mem_tracker = std::make_unique<MemTracker>();
        StoragePageCache::create_global_cache(_page_cache_mem_tracker.get(), 1000000000);
    }

    void TearDown() override {
        delete _block_mgr;
        delete _env;
        StoragePageCache::release_global_cache();
    }

    TabletSchema create_schema(const std::vector<TabletColumn>& columns, int num_short_key_columns = -1) {
        TabletSchema res;
        int num_key_columns = 0;
        for (auto& col : columns) {
            if (col.is_key()) {
                num_key_columns++;
            }
            res._cols.push_back(col);
        }
        res._num_key_columns = num_key_columns;
        res._num_short_key_columns = num_short_key_columns != -1 ? num_short_key_columns : num_key_columns;
        return res;
    }

    void build_segment(SegmentWriterOptions opts, const TabletSchema& build_schema, const TabletSchema& query_schema,
                       size_t nrows, const ValueGenerator& generator, shared_ptr<Segment>* res) {
        static int seg_id = 0;
        // must use unique filename for each segment, otherwise page cache kicks in and produces
        // the wrong answer (it use (filename,offset) as cache key)
        std::string filename = strings::Substitute("$0/seg_$1.dat", kSegmentDir, seg_id++);
        std::unique_ptr<fs::WritableBlock> wblock;
        fs::CreateBlockOptions block_opts({filename});
        ASSERT_OK(_block_mgr->create_block(block_opts, &wblock));
        SegmentWriter writer(std::move(wblock), 0, &build_schema, opts);
        ASSERT_OK(writer.init());

        auto schema = vectorized::ChunkHelper::convert_schema_to_format_v2(build_schema);
        auto chunk = vectorized::ChunkHelper::new_chunk(schema, nrows);
        for (size_t rid = 0; rid < nrows; ++rid) {
            auto& cols = chunk->columns();
            for (int cid = 0; cid < build_schema.num_columns(); ++cid) {
                int row_block_id = rid / opts.num_rows_per_block;
                cols[cid]->append_datum(generator(rid, cid, row_block_id));
            }
        }
        ASSERT_OK(writer.append_chunk(*chunk));

        uint64_t file_size, index_size;
        ASSERT_OK(writer.finalize(&file_size, &index_size));

        *res = *Segment::open(_tablet_meta_mem_tracker.get(), _block_mgr, filename, 0, &query_schema);
        ASSERT_EQ(nrows, (*res)->num_rows());
    }

    const std::string kSegmentDir = "/segment_test";

    EnvMemory* _env = nullptr;
    fs::FileBlockManager* _block_mgr = nullptr;
    std::unique_ptr<MemTracker> _page_cache_mem_tracker = nullptr;
    std::unique_ptr<MemTracker> _tablet_meta_mem_tracker = nullptr;
};

TEST_F(SegmentReaderWriterTest, estimate_segment_size) {
    size_t num_rows_per_block = 10;

    std::shared_ptr<TabletSchema> tablet_schema(new TabletSchema());
    tablet_schema->_num_key_columns = 3;
    tablet_schema->_num_short_key_columns = 2;
    tablet_schema->_num_rows_per_row_block = num_rows_per_block;
    tablet_schema->_cols.push_back(create_int_key(1));
    tablet_schema->_cols.push_back(create_int_key(2));
    tablet_schema->_cols.push_back(create_int_key(3));
    tablet_schema->_cols.push_back(create_int_value(4));

    // segment write
    std::string dname = "/segment_write_size";
    ASSERT_OK(_env->create_dir(dname));

    SegmentWriterOptions opts;
    opts.num_rows_per_block = num_rows_per_block;

    std::string fname = dname + "/int_case";
    std::unique_ptr<fs::WritableBlock> wblock;
    fs::CreateBlockOptions wblock_opts({fname});
    ASSERT_OK(_block_mgr->create_block(wblock_opts, &wblock));
    SegmentWriter writer(std::move(wblock), 0, tablet_schema.get(), opts);
    ASSERT_OK(writer.init());

    // 0, 1, 2, 3
    // 10, 11, 12, 13
    // 20, 21, 22, 23
    size_t nrows = 1048576;
    auto schema = vectorized::ChunkHelper::convert_schema_to_format_v2(*tablet_schema);
    auto chunk = vectorized::ChunkHelper::new_chunk(schema, nrows);
    for (size_t rid = 0; rid < nrows; ++rid) {
        auto& cols = chunk->columns();
        for (int cid = 0; cid < tablet_schema->num_columns(); ++cid) {
            cols[cid]->append_datum(vectorized::Datum(static_cast<int32_t>(rid * 10 + cid)));
        }
    }
    ASSERT_OK(writer.append_chunk(*chunk));

    uint32_t segment_size = writer.estimate_segment_size();
    LOG(INFO) << "estimated segment sizes=" << segment_size;

    uint64_t file_size = 0;
    uint64_t index_size;
    ASSERT_OK(writer.finalize(&file_size, &index_size));

    ASSERT_OK(_env->get_file_size(fname, &file_size));
    LOG(INFO) << "segment file size=" << file_size;

    ASSERT_NE(segment_size, 0);
}

TEST_F(SegmentReaderWriterTest, TestBloomFilterIndexUniqueModel) {
    TabletSchema schema = create_schema({create_int_key(1), create_int_key(2), create_int_key(3),
                                         create_int_value(4, OLAP_FIELD_AGGREGATION_REPLACE, true, "", true)});

    // for not base segment
    SegmentWriterOptions opts1;
    shared_ptr<Segment> seg1;
    build_segment(opts1, schema, schema, 100, DefaultIntGenerator, &seg1);
    ASSERT_TRUE(seg1->column(3)->has_bloom_filter_index());

    // for base segment
    SegmentWriterOptions opts2;
    shared_ptr<Segment> seg2;
    build_segment(opts2, schema, schema, 100, DefaultIntGenerator, &seg2);
    ASSERT_TRUE(seg2->column(3)->has_bloom_filter_index());
}

TEST_F(SegmentReaderWriterTest, TestHorizontalWrite) {
    TabletSchema tablet_schema =
            create_schema({create_int_key(1), create_int_key(2), create_int_value(3), create_int_value(4)});

    SegmentWriterOptions opts;
    opts.num_rows_per_block = 10;

    std::string file_name = kSegmentDir + "/horizontal_write_case";
    std::unique_ptr<fs::WritableBlock> wblock;
    fs::CreateBlockOptions wblock_opts({file_name});
    ASSERT_OK(_block_mgr->create_block(wblock_opts, &wblock));

    SegmentWriter writer(std::move(wblock), 0, &tablet_schema, opts);
    ASSERT_OK(writer.init());

    int32_t chunk_size = config::vector_chunk_size;
    size_t num_rows = 10000;
    auto schema = vectorized::ChunkHelper::convert_schema_to_format_v2(tablet_schema);
    auto chunk = vectorized::ChunkHelper::new_chunk(schema, chunk_size);
    for (auto i = 0; i < num_rows % chunk_size; ++i) {
        chunk->reset();
        auto& cols = chunk->columns();
        for (auto j = 0; j < chunk_size; ++j) {
            if (i * chunk_size + j >= num_rows) {
                break;
            }
            cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j)));
            cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j + 1)));
            cols[2]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j + 2)));
            cols[3]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j + 3)));
        }
        ASSERT_OK(writer.append_chunk(*chunk));
    }

    uint64_t file_size = 0;
    uint64_t index_size;
    ASSERT_OK(writer.finalize(&file_size, &index_size));

    auto segment = *Segment::open(_tablet_meta_mem_tracker.get(), _block_mgr, file_name, 0, &tablet_schema);
    ASSERT_EQ(segment->num_rows(), num_rows);

    vectorized::SegmentReadOptions seg_options;
    seg_options.block_mgr = _block_mgr;
    OlapReaderStatistics stats;
    seg_options.stats = &stats;
    auto res = segment->new_iterator(schema, seg_options);
    ASSERT_FALSE(res.status().is_end_of_file() || !res.ok() || res.value() == nullptr);
    auto seg_iterator = res.value();

    size_t count = 0;
    while (true) {
        chunk->reset();
        auto st = seg_iterator->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_FALSE(!st.ok());
        for (auto i = 0; i < chunk->num_rows(); ++i) {
            EXPECT_EQ(count, chunk->get(i)[0].get_int32());
            EXPECT_EQ(count + 1, chunk->get(i)[1].get_int32());
            EXPECT_EQ(count + 2, chunk->get(i)[2].get_int32());
            EXPECT_EQ(count + 3, chunk->get(i)[3].get_int32());
            ++count;
        }
    }
    EXPECT_EQ(count, num_rows);
}

TEST_F(SegmentReaderWriterTest, TestVerticalWrite) {
    TabletSchema tablet_schema =
            create_schema({create_int_key(1), create_int_key(2), create_int_value(3), create_int_value(4)});

    SegmentWriterOptions opts;
    opts.num_rows_per_block = 10;

    std::string file_name = kSegmentDir + "/vertical_write_case";
    std::unique_ptr<fs::WritableBlock> wblock;
    fs::CreateBlockOptions wblock_opts({file_name});
    ASSERT_OK(_block_mgr->create_block(wblock_opts, &wblock));

    SegmentWriter writer(std::move(wblock), 0, &tablet_schema, opts);

    int32_t chunk_size = config::vector_chunk_size;
    size_t num_rows = 10000;
    uint64_t file_size = 0;
    uint64_t index_size = 0;

    {
        // col1 col2
        std::vector<uint32_t> column_indexes{0, 1};
        ASSERT_OK(writer.init(column_indexes, true));
        auto schema = vectorized::ChunkHelper::convert_schema_to_format_v2(tablet_schema, column_indexes);
        auto chunk = vectorized::ChunkHelper::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto& cols = chunk->columns();
            for (auto j = 0; j < chunk_size; ++j) {
                if (i * chunk_size + j >= num_rows) {
                    break;
                }
                cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j)));
                cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j + 1)));
            }
            ASSERT_OK(writer.append_chunk(*chunk));
        }
        ASSERT_OK(writer.finalize_columns(&index_size));
    }

    {
        // col3
        std::vector<uint32_t> column_indexes{2};
        ASSERT_OK(writer.init(column_indexes, false));
        auto schema = vectorized::ChunkHelper::convert_schema_to_format_v2(tablet_schema, column_indexes);
        auto chunk = vectorized::ChunkHelper::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto& cols = chunk->columns();
            for (auto j = 0; j < chunk_size; ++j) {
                if (i * chunk_size + j >= num_rows) {
                    break;
                }
                cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j + 2)));
            }
            ASSERT_OK(writer.append_chunk(*chunk));
        }
        ASSERT_OK(writer.finalize_columns(&index_size));
    }

    {
        // col4
        std::vector<uint32_t> column_indexes{3};
        ASSERT_OK(writer.init(column_indexes, false));
        auto schema = vectorized::ChunkHelper::convert_schema_to_format_v2(tablet_schema, column_indexes);
        auto chunk = vectorized::ChunkHelper::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto& cols = chunk->columns();
            for (auto j = 0; j < chunk_size; ++j) {
                if (i * chunk_size + j >= num_rows) {
                    break;
                }
                cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j + 3)));
            }
            ASSERT_OK(writer.append_chunk(*chunk));
        }
        ASSERT_OK(writer.finalize_columns(&index_size));
    }

    ASSERT_OK(writer.finalize_footer(&file_size));

    auto segment = *Segment::open(_tablet_meta_mem_tracker.get(), _block_mgr, file_name, 0, &tablet_schema);
    ASSERT_EQ(segment->num_rows(), num_rows);

    vectorized::SegmentReadOptions seg_options;
    seg_options.block_mgr = _block_mgr;
    OlapReaderStatistics stats;
    seg_options.stats = &stats;
    auto schema = vectorized::ChunkHelper::convert_schema_to_format_v2(tablet_schema);
    auto res = segment->new_iterator(schema, seg_options);
    ASSERT_FALSE(res.status().is_end_of_file() || !res.ok() || res.value() == nullptr);
    auto seg_iterator = res.value();

    size_t count = 0;
    auto chunk = vectorized::ChunkHelper::new_chunk(schema, chunk_size);
    while (true) {
        chunk->reset();
        auto st = seg_iterator->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_FALSE(!st.ok());
        for (auto i = 0; i < chunk->num_rows(); ++i) {
            EXPECT_EQ(count, chunk->get(i)[0].get_int32());
            EXPECT_EQ(count + 1, chunk->get(i)[1].get_int32());
            EXPECT_EQ(count + 2, chunk->get(i)[2].get_int32());
            EXPECT_EQ(count + 3, chunk->get(i)[3].get_int32());
            ++count;
        }
    }
    EXPECT_EQ(count, num_rows);
}

TEST_F(SegmentReaderWriterTest, TestReadMultipleTypesColumn) {
    std::string s1("abcdefghijklmnopqrstuvwxyz");
    std::string s2("bbcdefghijklmnopqrstuvwxyz");
    std::string s3("cbcdefghijklmnopqrstuvwxyz");
    std::string s4("dbcdefghijklmnopqrstuvwxyz");
    std::string s5("ebcdefghijklmnopqrstuvwxyz");
    std::string s6("fbcdefghijklmnopqrstuvwxyz");
    std::string s7("gbcdefghijklmnopqrstuvwxyz");
    std::string s8("hbcdefghijklmnopqrstuvwxyz");
    std::vector<Slice> data_strs{s1, s2, s3, s4, s5, s6, s7, s8};

    TabletColumn c1 = create_int_key(1);
    TabletColumn c2 = create_int_key(2);
    TabletColumn c3 = create_with_default_value<OLAP_FIELD_TYPE_VARCHAR>("");
    c3.set_length(65535);

    TabletSchema tablet_schema = create_schema({c1, c2, c3});

    SegmentWriterOptions opts;
    opts.num_rows_per_block = 10;

    std::string file_name = kSegmentDir + "/read_multiple_types_column";
    std::unique_ptr<fs::WritableBlock> wblock;
    fs::CreateBlockOptions wblock_opts({file_name});
    ASSERT_OK(_block_mgr->create_block(wblock_opts, &wblock));

    SegmentWriter writer(std::move(wblock), 0, &tablet_schema, opts);

    int32_t chunk_size = config::vector_chunk_size;
    size_t num_rows = 10000;
    uint64_t file_size = 0;
    uint64_t index_size = 0;

    {
        // col1 col2
        std::vector<uint32_t> column_indexes{0, 1};
        ASSERT_OK(writer.init(column_indexes, true));
        auto schema = vectorized::ChunkHelper::convert_schema_to_format_v2(tablet_schema, column_indexes);
        auto chunk = vectorized::ChunkHelper::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto& cols = chunk->columns();
            for (auto j = 0; j < chunk_size; ++j) {
                if (i * chunk_size + j >= num_rows) {
                    break;
                }
                cols[0]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j)));
                cols[1]->append_datum(vectorized::Datum(static_cast<int32_t>(i * chunk_size + j + 1)));
            }
            ASSERT_OK(writer.append_chunk(*chunk));
        }
        ASSERT_OK(writer.finalize_columns(&index_size));
    }

    {
        // col3
        std::vector<uint32_t> column_indexes{2};
        ASSERT_OK(writer.init(column_indexes, false));
        auto schema = vectorized::ChunkHelper::convert_schema_to_format_v2(tablet_schema, column_indexes);
        auto chunk = vectorized::ChunkHelper::new_chunk(schema, chunk_size);
        for (auto i = 0; i < num_rows % chunk_size; ++i) {
            chunk->reset();
            auto& cols = chunk->columns();
            for (auto j = 0; j < chunk_size; ++j) {
                if (i * chunk_size + j >= num_rows) {
                    break;
                }
                cols[0]->append_datum(vectorized::Datum(data_strs[j % 8]));
            }
            ASSERT_OK(writer.append_chunk(*chunk));
        }
        ASSERT_OK(writer.finalize_columns(&index_size));
    }

    ASSERT_OK(writer.finalize_footer(&file_size));
    auto segment = *Segment::open(_tablet_meta_mem_tracker.get(), _block_mgr, file_name, 0, &tablet_schema);
    ASSERT_EQ(segment->num_rows(), num_rows);

    vectorized::SegmentReadOptions seg_options;
    seg_options.block_mgr = _block_mgr;
    OlapReaderStatistics stats;
    seg_options.stats = &stats;
    auto schema = vectorized::ChunkHelper::convert_schema_to_format_v2(tablet_schema);
    auto res = segment->new_iterator(schema, seg_options);
    ASSERT_FALSE(res.status().is_end_of_file() || !res.ok() || res.value() == nullptr);
    auto seg_iterator = res.value();

    size_t count = 0;
    auto chunk = vectorized::ChunkHelper::new_chunk(schema, chunk_size);
    while (true) {
        chunk->reset();
        auto st = seg_iterator->get_next(chunk.get());
        if (st.is_end_of_file()) {
            break;
        }
        ASSERT_FALSE(!st.ok());
        for (auto i = 0; i < chunk->num_rows(); ++i) {
            EXPECT_EQ(count, chunk->get(i)[0].get_int32());
            EXPECT_EQ(count + 1, chunk->get(i)[1].get_int32());
            EXPECT_EQ(data_strs[i % 8].to_string(), chunk->get(i)[2].get_slice().to_string());
            ++count;
        }
    }
    EXPECT_EQ(count, num_rows);
}

} // namespace starrocks
