// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include <string>
#include <unordered_map>

#include "storage/olap_common.h"
#include "storage/primary_index.h"
#include "storage/tablet_updates.h"

namespace starrocks {

class Rowset;
using RowsetSharedPtr = std::shared_ptr<Rowset>;
class Tablet;
class TabletMeta;
using TabletSharedPtr = std::shared_ptr<Tablet>;

class RowsetUpdateState {
public:
    using ColumnUniquePtr = std::unique_ptr<vectorized::Column>;

    RowsetUpdateState();
    ~RowsetUpdateState();

    Status load(Tablet* tablet, Rowset* rowset);

    Status apply(Tablet* tablet, Rowset* rowset, uint32_t rowset_id, const PrimaryIndex& index);

    const std::vector<ColumnUniquePtr>& upserts() const { return _upserts; }
    const std::vector<ColumnUniquePtr>& deletes() const { return _deletes; }

    std::size_t memory_usage() const { return _memory_usage; }

    std::string to_string() const;

private:
    Status _do_load(Tablet* tablet, Rowset* rowset);

    std::once_flag _load_once_flag;
    Status _status;
    // one for each segment file
    std::vector<ColumnUniquePtr> _upserts;
    // one for each delete file
    std::vector<ColumnUniquePtr> _deletes;
    size_t _memory_usage = 0;
    int64_t _tablet_id = 0;

    // states for partial update
    EditVersion _read_version;
    uint32_t _next_rowset_id = 0;
    struct PartialUpdateState {
        vector<uint64_t> src_rss_rowids;
        vector<std::unique_ptr<vectorized::Column>> write_columns;
    };
    // TODO: dump to disk if memory usage is too large
    std::vector<PartialUpdateState> _parital_update_states;

    RowsetUpdateState(const RowsetUpdateState&) = delete;
    const RowsetUpdateState& operator=(const RowsetUpdateState&) = delete;
};

inline std::ostream& operator<<(std::ostream& os, const RowsetUpdateState& o) {
    os << o.to_string();
    return os;
}

} // namespace starrocks
