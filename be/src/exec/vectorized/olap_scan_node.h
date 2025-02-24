// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "column/chunk.h"
#include "exec/olap_common.h"
#include "exec/scan_node.h"
#include "exec/vectorized/olap_scan_prepare.h"
#include "exec/vectorized/tablet_scanner.h"

namespace starrocks {
class DescriptorTbl;
class SlotDescriptor;
class TupleDescriptor;
} // namespace starrocks

namespace starrocks::vectorized {

// OlapScanNode fetch records from storage engine and pass them to the parent node.
// It will submit many TabletScanner to a global-shared thread pool to execute concurrently.
//
// Execution flow:
// 1. OlapScanNode creates many empty chunks and put them into _chunk_pool.
// 2. OlapScanNode submit many OlapScanners to a global-shared thread pool.
// 3. TabletScanner fetch an empty Chunk from _chunk_pool and fill it with the records retrieved
//    from storage engine.
// 4. TabletScanner put the non-empty Chunk into _result_chunks.
// 5. OlapScanNode receive chunk from _result_chunks and put an new empty chunk into _chunk_pool.
//
// If _chunk_pool is empty, OlapScanners will quit the thread pool and put themself to the
// _pending_scanners. After enough chunks has been placed into _chunk_pool, OlapScanNode will
// resubmit OlapScanners to the thread pool.
class OlapScanNode final : public starrocks::ScanNode {
public:
    OlapScanNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs);
    ~OlapScanNode() override;

    Status init(const TPlanNode& tnode, RuntimeState* state) override;
    Status prepare(RuntimeState* state) override;
    Status open(RuntimeState* state) override;
    Status get_next(RuntimeState* state, ChunkPtr* chunk, bool* eos) override;
    Status close(RuntimeState* statue) override;

    Status set_scan_ranges(const std::vector<TScanRangeParams>& scan_ranges) override;
    void debug_string(int indentation_level, std::stringstream* out) const override {
        *out << "vectorized::OlapScanNode";
    }
    Status collect_query_statistics(QueryStatistics* statistics) override;

    Status set_scan_ranges(const std::vector<TInternalScanRange>& ranges);

    Status set_scan_range(const TInternalScanRange& range);

    std::vector<std::shared_ptr<pipeline::OperatorFactory>> decompose_to_pipeline(
            pipeline::PipelineBuilderContext* context) override;

private:
    friend class TabletScanner;

    constexpr static const int kMaxConcurrency = 50;

    template <typename T>
    class Stack {
    public:
        void reserve(size_t n) { _items.reserve(n); }

        void push(const T& p) { _items.push_back(p); }

        void push(T&& v) { _items.emplace_back(std::move(v)); }

        void clear() { _items.clear(); }

        // REQUIRES: not empty.
        T pop() {
            DCHECK(!_items.empty());
            T v = _items.back();
            _items.pop_back();
            return v;
        }

        size_t size() const { return _items.size(); }

        bool empty() const { return _items.empty(); }

        void reverse() { std::reverse(_items.begin(), _items.end()); }

    private:
        std::vector<T> _items;
    };

    Status _start_scan(RuntimeState* state);
    Status _start_scan_thread(RuntimeState* state);
    void _scanner_thread(TabletScanner* scanner);

    void _init_counter(RuntimeState* state);

    void _update_status(const Status& status);
    Status _get_status();

    void _fill_chunk_pool(int count, bool force_column_pool);
    bool _submit_scanner(TabletScanner* scanner, bool blockable);
    void _close_pending_scanners();
    int _compute_priority(int32_t num_submitted_tasks);

    TOlapScanNode _olap_scan_node;
    std::vector<std::unique_ptr<TInternalScanRange>> _scan_ranges;
    RuntimeState* _runtime_state = nullptr;
    TupleDescriptor* _tuple_desc = nullptr;
    OlapScanConjunctsManager _conjuncts_manager;
    DictOptimizeParser _dict_optimize_parser;
    const Schema* _chunk_schema = nullptr;
    ObjectPool _obj_pool;

    int32_t _num_scanners = 0;
    int32_t _chunks_per_scanner = 10;
    bool _start = false;

    mutable SpinLock _status_mutex;
    Status _status;

    // _mtx protects _chunk_pool and _pending_scanners.
    std::mutex _mtx;
    Stack<ChunkPtr> _chunk_pool;
    Stack<TabletScanner*> _pending_scanners;

    UnboundedBlockingQueue<ChunkPtr> _result_chunks;

    // used to compute task priority.
    std::atomic<int32_t> _scanner_submit_count{0};
    std::atomic<int32_t> _running_threads{0};
    std::atomic<int32_t> _closed_scanners{0};

    std::vector<std::string> _unused_output_columns;

    // profile
    RuntimeProfile* _scan_profile = nullptr;

    RuntimeProfile::Counter* _scan_timer = nullptr;
    RuntimeProfile::Counter* _create_seg_iter_timer = nullptr;
    RuntimeProfile::Counter* _tablet_counter = nullptr;
    RuntimeProfile::Counter* _io_timer = nullptr;
    RuntimeProfile::Counter* _read_compressed_counter = nullptr;
    RuntimeProfile::Counter* _decompress_timer = nullptr;
    RuntimeProfile::Counter* _read_uncompressed_counter = nullptr;
    RuntimeProfile::Counter* _raw_rows_counter = nullptr;
    RuntimeProfile::Counter* _pred_filter_counter = nullptr;
    RuntimeProfile::Counter* _del_vec_filter_counter = nullptr;
    RuntimeProfile::Counter* _pred_filter_timer = nullptr;
    RuntimeProfile::Counter* _chunk_copy_timer = nullptr;
    RuntimeProfile::Counter* _seg_init_timer = nullptr;
    RuntimeProfile::Counter* _seg_zm_filtered_counter = nullptr;
    RuntimeProfile::Counter* _zm_filtered_counter = nullptr;
    RuntimeProfile::Counter* _bf_filtered_counter = nullptr;
    RuntimeProfile::Counter* _sk_filtered_counter = nullptr;
    RuntimeProfile::Counter* _block_seek_timer = nullptr;
    RuntimeProfile::Counter* _block_seek_counter = nullptr;
    RuntimeProfile::Counter* _block_load_timer = nullptr;
    RuntimeProfile::Counter* _block_load_counter = nullptr;
    RuntimeProfile::Counter* _block_fetch_timer = nullptr;
    RuntimeProfile::Counter* _index_load_timer = nullptr;
    RuntimeProfile::Counter* _read_pages_num_counter = nullptr;
    RuntimeProfile::Counter* _cached_pages_num_counter = nullptr;
    RuntimeProfile::Counter* _bi_filtered_counter = nullptr;
    RuntimeProfile::Counter* _bi_filter_timer = nullptr;
    RuntimeProfile::Counter* _pushdown_predicates_counter = nullptr;
    RuntimeProfile::Counter* _rowsets_read_count = nullptr;
    RuntimeProfile::Counter* _segments_read_count = nullptr;
    RuntimeProfile::Counter* _total_columns_data_page_count = nullptr;
};

} // namespace starrocks::vectorized
