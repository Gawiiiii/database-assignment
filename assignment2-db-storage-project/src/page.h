#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

struct RID {
    uint32_t page_id = 0;
    uint16_t slot_id = 0;
};

inline bool operator==(const RID& a, const RID& b) {
    return a.page_id == b.page_id && a.slot_id == b.slot_id;
}

struct Slot {
    uint16_t record_offset = 0;
    uint16_t record_length = 0;
    bool is_deleted = false;
};

class Page {
public:
    static constexpr uint16_t kPageSize = 4096;
    static constexpr uint16_t kHeaderSize = 16;
    static constexpr uint16_t kSlotSize = 8;

    explicit Page(uint32_t page_id);

    bool can_insert(std::size_t record_len) const;
    uint16_t insert_record(const std::vector<uint8_t>& record_bytes);
    std::optional<std::vector<uint8_t>> read_record(uint16_t slot_id) const;
    bool delete_record(uint16_t slot_id);
    bool update_in_place(uint16_t slot_id, const std::vector<uint8_t>& record_bytes);

    uint32_t page_id() const { return page_id_; }
    uint16_t slot_count() const { return static_cast<uint16_t>(slots_.size()); }
    uint16_t free_space_offset() const { return free_space_offset_; }
    uint16_t free_space_size() const;
    const Slot* slot(uint16_t slot_id) const;

private:
    uint32_t page_id_;
    uint16_t free_space_offset_;
    std::array<uint8_t, kPageSize> data_{};
    std::vector<Slot> slots_;
};
