#include "record.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {

void write_u16(std::vector<uint8_t>& out, std::size_t pos, uint16_t value) {
    out[pos] = static_cast<uint8_t>(value & 0xffu);
    out[pos + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

void write_u32(std::vector<uint8_t>& out, std::size_t pos, uint32_t value) {
    out[pos] = static_cast<uint8_t>(value & 0xffu);
    out[pos + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    out[pos + 2] = static_cast<uint8_t>((value >> 16u) & 0xffu);
    out[pos + 3] = static_cast<uint8_t>((value >> 24u) & 0xffu);
}

uint16_t read_u16(const std::vector<uint8_t>& in, std::size_t pos) {
    return static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8u));
}

uint32_t read_u32(const std::vector<uint8_t>& in, std::size_t pos) {
    return static_cast<uint32_t>(in[pos]) |
           (static_cast<uint32_t>(in[pos + 1]) << 8u) |
           (static_cast<uint32_t>(in[pos + 2]) << 16u) |
           (static_cast<uint32_t>(in[pos + 3]) << 24u);
}

std::string trim_zero_padding(const uint8_t* data, std::size_t len) {
    std::size_t end = len;
    while (end > 0 && data[end - 1] == 0) {
        --end;
    }
    return std::string(reinterpret_cast<const char*>(data), end);
}

int32_t require_int(const FieldValue& value, const std::string& field_name) {
    if (auto p = std::get_if<int32_t>(&value)) {
        return *p;
    }
    if (auto s = std::get_if<std::string>(&value)) {
        try {
            return static_cast<int32_t>(std::stol(*s));
        } catch (...) {
            throw std::runtime_error("field '" + field_name + "' expects INT but got '" + *s + "'");
        }
    }
    throw std::runtime_error("field '" + field_name + "' expects INT");
}

std::string require_string(const FieldValue& value, const std::string& field_name) {
    if (auto p = std::get_if<std::string>(&value)) {
        return *p;
    }
    if (auto i = std::get_if<int32_t>(&value)) {
        return std::to_string(*i);
    }
    throw std::runtime_error("field '" + field_name + "' expects string value");
}

} // namespace

std::vector<uint8_t> RecordCodec::serialize(const TableSchema& schema, const Record& record) {
    if (record.values.size() != schema.fields.size()) {
        throw std::runtime_error("record field count mismatch: expected " +
                                 std::to_string(schema.fields.size()) + ", got " +
                                 std::to_string(record.values.size()));
    }

    const std::size_t fixed_size = schema.fixed_area_size();
    std::vector<uint8_t> out(kHeaderSize + fixed_size, 0);
    write_u16(out, 0, static_cast<uint16_t>(schema.fields.size()));
    write_u16(out, 2, 0);

    std::size_t fixed_pos = kHeaderSize;
    std::vector<uint8_t> var_data;

    for (std::size_t i = 0; i < schema.fields.size(); ++i) {
        const auto& field = schema.fields[i];
        const auto& value = record.values[i];
        if (field.type == FieldType::Int) {
            const int32_t v = require_int(value, field.name);
            write_u32(out, fixed_pos, static_cast<uint32_t>(v));
            fixed_pos += 4;
        } else if (field.type == FieldType::Char) {
            const std::string s = require_string(value, field.name);
            const std::size_t copy_len = std::min(field.length, s.size());
            std::memcpy(out.data() + fixed_pos, s.data(), copy_len);
            fixed_pos += field.length;
        } else {
            std::string s = require_string(value, field.name);
            if (s.size() > field.length) {
                s.resize(field.length);
            }
            const uint32_t offset = static_cast<uint32_t>(kHeaderSize + fixed_size + var_data.size());
            const uint32_t length = static_cast<uint32_t>(s.size());
            write_u32(out, fixed_pos, offset);
            write_u32(out, fixed_pos + 4, length);
            fixed_pos += 8;
            var_data.insert(var_data.end(), s.begin(), s.end());
        }
    }

    out.insert(out.end(), var_data.begin(), var_data.end());
    write_u32(out, 4, static_cast<uint32_t>(out.size()));
    return out;
}

Record RecordCodec::deserialize(const TableSchema& schema, const std::vector<uint8_t>& bytes) {
    if (bytes.size() < kHeaderSize) {
        throw std::runtime_error("record is too small to contain header");
    }
    const auto field_count = read_u16(bytes, 0);
    const auto total_size = read_u32(bytes, 4);
    if (field_count != schema.fields.size()) {
        throw std::runtime_error("record field count does not match schema");
    }
    if (total_size != bytes.size()) {
        throw std::runtime_error("record total_size does not match byte vector size");
    }

    Record record;
    record.values.reserve(schema.fields.size());
    std::size_t fixed_pos = kHeaderSize;
    for (const auto& field : schema.fields) {
        if (field.type == FieldType::Int) {
            if (fixed_pos + 4 > bytes.size()) {
                throw std::runtime_error("corrupt INT field in record");
            }
            record.values.emplace_back(static_cast<int32_t>(read_u32(bytes, fixed_pos)));
            fixed_pos += 4;
        } else if (field.type == FieldType::Char) {
            if (fixed_pos + field.length > bytes.size()) {
                throw std::runtime_error("corrupt CHAR field in record");
            }
            record.values.emplace_back(trim_zero_padding(bytes.data() + fixed_pos, field.length));
            fixed_pos += field.length;
        } else {
            if (fixed_pos + 8 > bytes.size()) {
                throw std::runtime_error("corrupt VARCHAR directory entry in record");
            }
            const auto offset = read_u32(bytes, fixed_pos);
            const auto length = read_u32(bytes, fixed_pos + 4);
            fixed_pos += 8;
            if (static_cast<std::size_t>(offset) + length > bytes.size()) {
                throw std::runtime_error("corrupt VARCHAR payload in record");
            }
            record.values.emplace_back(std::string(reinterpret_cast<const char*>(bytes.data() + offset), length));
        }
    }
    return record;
}

std::string RecordCodec::primary_key_to_string(const TableSchema& schema, const Record& record) {
    const int idx = schema.primary_key_index();
    if (idx < 0 || static_cast<std::size_t>(idx) >= record.values.size()) {
        throw std::runtime_error("primary key field is not available in record");
    }
    const auto& value = record.values[static_cast<std::size_t>(idx)];
    if (auto p = std::get_if<int32_t>(&value)) {
        return std::to_string(*p);
    }
    return std::get<std::string>(value);
}
