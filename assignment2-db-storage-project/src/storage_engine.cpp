#include "storage_engine.h"

#include <stdexcept>

StorageEngine::StorageEngine(TableSchema schema, std::size_t btree_order)
    : schema_(std::move(schema)), primary_index_(btree_order) {
    append_page();
}

RID StorageEngine::insert(const Record& record) {
    const auto bytes = RecordCodec::serialize(schema_, record);
    if (bytes.size() + Page::kHeaderSize + Page::kSlotSize > Page::kPageSize) {
        throw std::runtime_error("record is larger than a 4KB page");
    }

    Page* target = &pages_.back();
    if (!target->can_insert(bytes.size())) {
        target = &append_page();
    }

    const auto slot_id = target->insert_record(bytes);
    RID rid{target->page_id(), slot_id};
    primary_index_.insert(key_of(record), rid);
    io_stats_.logical_write_bytes += bytes.size();
    return rid;
}

std::optional<Record> StorageEngine::read(const RID& rid) {
    if (rid.page_id >= pages_.size()) {
        return std::nullopt;
    }
    auto bytes = pages_[rid.page_id].read_record(rid.slot_id);
    if (!bytes) {
        return std::nullopt;
    }
    io_stats_.logical_read_bytes += bytes->size();
    return RecordCodec::deserialize(schema_, *bytes);
}

std::optional<Record> StorageEngine::read_by_key(const std::string& key) {
    auto rid = primary_index_.search(key);
    if (!rid) {
        return std::nullopt;
    }
    return read(*rid);
}

std::optional<RID> StorageEngine::find_rid_by_key(const std::string& key) const {
    return primary_index_.search(key);
}

bool StorageEngine::erase(const RID& rid) {
    if (rid.page_id >= pages_.size()) {
        return false;
    }
    auto old = read(rid);
    if (!old) {
        return false;
    }
    if (!pages_[rid.page_id].delete_record(rid.slot_id)) {
        return false;
    }
    primary_index_.remove(key_of(*old));
    io_stats_.logical_write_bytes += 16;
    return true;
}

bool StorageEngine::erase_by_key(const std::string& key) {
    auto rid = primary_index_.search(key);
    if (!rid) {
        return false;
    }
    return erase(*rid);
}

bool StorageEngine::update(const RID& rid, const Record& new_record) {
    if (rid.page_id >= pages_.size()) {
        return false;
    }
    auto old = read(rid);
    if (!old) {
        return false;
    }
    const auto old_key = key_of(*old);
    const auto new_key = key_of(new_record);
    const auto new_bytes = RecordCodec::serialize(schema_, new_record);

    if (pages_[rid.page_id].update_in_place(rid.slot_id, new_bytes)) {
        if (old_key != new_key) {
            primary_index_.remove(old_key);
            primary_index_.insert(new_key, rid);
        } else {
            primary_index_.update(new_key, rid);
        }
        io_stats_.logical_write_bytes += new_bytes.size();
        return true;
    }

    if (!pages_[rid.page_id].delete_record(rid.slot_id)) {
        return false;
    }
    primary_index_.remove(old_key);
    RID new_rid = insert(new_record);
    primary_index_.update(new_key, new_rid);
    return true;
}

bool StorageEngine::update_by_key(const std::string& key, const Record& new_record) {
    auto rid = primary_index_.search(key);
    if (!rid) {
        return false;
    }
    return update(*rid, new_record);
}

std::string StorageEngine::key_of(const Record& record) const {
    return RecordCodec::primary_key_to_string(schema_, record);
}

Page& StorageEngine::append_page() {
    pages_.emplace_back(static_cast<uint32_t>(pages_.size()));
    return pages_.back();
}
