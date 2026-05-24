#pragma once

#include "schema.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

using FieldValue = std::variant<int32_t, std::string>;

struct Record {
    std::vector<FieldValue> values;
};

class RecordCodec {
public:
    static constexpr std::size_t kHeaderSize = 8;

    static std::vector<uint8_t> serialize(const TableSchema& schema, const Record& record);
    static Record deserialize(const TableSchema& schema, const std::vector<uint8_t>& bytes);
    static std::string primary_key_to_string(const TableSchema& schema, const Record& record);
};
