#include "page.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

Page::Page(uint32_t page_id)
    : page_id_(page_id), free_space_offset_(kPageSize) {}

bool Page::can_insert(std::size_t record_len) const {
    if (record_len > kPageSize) {
        return false;
    }
    const auto slot_dir_end = kHeaderSize + (slots_.size() + 1) * kSlotSize;
    return slot_dir_end + record_len <= free_space_offset_;
}

uint16_t Page::insert_record(const std::vector<uint8_t>& record_bytes) {
    if (!can_insert(record_bytes.size())) {
        throw std::runtime_error("page " + std::to_string(page_id_) + " has insufficient free space");
    }
    free_space_offset_ = static_cast<uint16_t>(free_space_offset_ - record_bytes.size());
    std::memcpy(data_.data() + free_space_offset_, record_bytes.data(), record_bytes.size());

    Slot slot;
    slot.record_offset = free_space_offset_;
    slot.record_length = static_cast<uint16_t>(record_bytes.size());
    slot.is_deleted = false;
    slots_.push_back(slot);
    return static_cast<uint16_t>(slots_.size() - 1);
}

std::optional<std::vector<uint8_t>> Page::read_record(uint16_t slot_id) const {
    if (slot_id >= slots_.size()) {
        return std::nullopt;
    }
    const auto& slot = slots_[slot_id];
    if (slot.is_deleted) {
        return std::nullopt;
    }
    return std::vector<uint8_t>(data_.begin() + slot.record_offset,
                                data_.begin() + slot.record_offset + slot.record_length);
}

bool Page::delete_record(uint16_t slot_id) {
    if (slot_id >= slots_.size() || slots_[slot_id].is_deleted) {
        return false;
    }
    slots_[slot_id].is_deleted = true;
    return true;
}

bool Page::update_in_place(uint16_t slot_id, const std::vector<uint8_t>& record_bytes) {
    if (slot_id >= slots_.size() || slots_[slot_id].is_deleted) {
        return false;
    }
    auto& slot = slots_[slot_id];
    if (record_bytes.size() > slot.record_length) {
        return false;
    }
    std::memcpy(data_.data() + slot.record_offset, record_bytes.data(), record_bytes.size());
    slot.record_length = static_cast<uint16_t>(record_bytes.size());
    return true;
}

uint16_t Page::free_space_size() const {
    const auto slot_dir_end = kHeaderSize + slots_.size() * kSlotSize;
    if (slot_dir_end >= free_space_offset_) {
        return 0;
    }
    return static_cast<uint16_t>(free_space_offset_ - slot_dir_end);
}

const Slot* Page::slot(uint16_t slot_id) const {
    if (slot_id >= slots_.size()) {
        return nullptr;
    }
    return &slots_[slot_id];
}
