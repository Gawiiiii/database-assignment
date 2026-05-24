#pragma once

#include "bplustree.h"
#include "page.h"
#include "record.h"
#include "schema.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct IOStats {
    uint64_t logical_read_bytes = 0;
    uint64_t logical_write_bytes = 0;
};

class StorageEngine {
public:
    explicit StorageEngine(TableSchema schema, std::size_t btree_order = 128);

    RID insert(const Record& record);
    std::optional<Record> read(const RID& rid);
    std::optional<Record> read_by_key(const std::string& key);
    std::optional<RID> find_rid_by_key(const std::string& key) const;
    bool erase(const RID& rid);
    bool erase_by_key(const std::string& key);
    bool update(const RID& rid, const Record& new_record);
    bool update_by_key(const std::string& key, const Record& new_record);

    std::size_t page_count() const { return pages_.size(); }
    std::size_t estimated_storage_bytes() const { return pages_.size() * Page::kPageSize; }
    const IOStats& io_stats() const { return io_stats_; }
    const TableSchema& schema() const { return schema_; }

private:
    std::string key_of(const Record& record) const;
    Page& append_page();

    TableSchema schema_;
    std::vector<Page> pages_;
    BPlusTree<std::string, RID> primary_index_;
    IOStats io_stats_;
};
