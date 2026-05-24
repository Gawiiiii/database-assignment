#include "tpch_loader.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

std::vector<std::string> split_tbl_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    for (char c : line) {
        if (c == '|') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        fields.push_back(current);
    }
    return fields;
}

std::string zero_pad(std::size_t value, int width) {
    std::ostringstream oss;
    oss << std::setw(width) << std::setfill('0') << value;
    return oss.str();
}

int32_t parse_int_or_zero(const std::string& s) {
    try {
        return static_cast<int32_t>(std::stol(s));
    } catch (...) {
        return 0;
    }
}

} // namespace

bool load_tpch_orders(const std::string& tpch_dir,
                      const TableSchema& schema,
                      std::size_t limit,
                      const std::function<void(const Record&)>& consumer) {
    std::filesystem::path path = std::filesystem::path(tpch_dir) / "orders.tbl";
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream in(path);
    if (!in) {
        return false;
    }

    const std::vector<std::string> expected = {
        "o_orderkey", "o_custkey", "o_orderstatus", "o_totalprice", "o_orderdate",
        "o_orderpriority", "o_clerk", "o_shippriority", "o_comment"
    };
    for (const auto& name : expected) {
        if (schema.field_index(name) < 0) {
            throw std::runtime_error("orders schema is missing field '" + name + "'");
        }
    }

    std::string line;
    std::size_t count = 0;
    while (count < limit && std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto parts = split_tbl_line(line);
        if (parts.size() < expected.size()) {
            throw std::runtime_error("orders.tbl line has fewer than 9 fields");
        }

        Record record;
        record.values.resize(schema.fields.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            int idx = schema.field_index(expected[i]);
            const auto& field = schema.fields[static_cast<std::size_t>(idx)];
            if (field.type == FieldType::Int) {
                record.values[static_cast<std::size_t>(idx)] = parse_int_or_zero(parts[i]);
            } else {
                record.values[static_cast<std::size_t>(idx)] = parts[i];
            }
        }
        consumer(record);
        ++count;
    }
    return count > 0;
}

Record make_synthetic_record(const TableSchema& schema, std::size_t id) {
    Record record;
    record.values.reserve(schema.fields.size());
    const std::string padded = zero_pad(id, 10);

    for (const auto& field : schema.fields) {
        if (field.type == FieldType::Int) {
            if (field.name == "Age" || field.name == "age") {
                record.values.emplace_back(static_cast<int32_t>(18 + (id % 13)));
            } else {
                record.values.emplace_back(static_cast<int32_t>(id));
            }
        } else if (field.type == FieldType::Char) {
            if (field.name == "Sno") {
                record.values.emplace_back(padded);
            } else if (field.name == "Sname") {
                record.values.emplace_back("Name_" + padded);
            } else if (field.name == "o_orderstatus") {
                record.values.emplace_back((id % 3 == 0) ? "F" : ((id % 3 == 1) ? "O" : "P"));
            } else if (field.name == "o_orderdate") {
                record.values.emplace_back("1996-01-" + zero_pad((id % 28) + 1, 2));
            } else {
                record.values.emplace_back("C" + padded);
            }
        } else {
            if (field.name == "Address") {
                record.values.emplace_back("City_" + std::to_string(id % 100) + "_Street_" + padded);
            } else if (field.name == "o_totalprice") {
                record.values.emplace_back(std::to_string(1000 + (id % 100000)) + ".00");
            } else if (field.name == "o_orderpriority") {
                record.values.emplace_back(std::to_string(1 + (id % 5)) + "-URGENT");
            } else if (field.name == "o_clerk") {
                record.values.emplace_back("Clerk#" + zero_pad(id % 1000000, 9));
            } else if (field.name == "o_comment") {
                record.values.emplace_back("synthetic order comment " + padded);
            } else {
                record.values.emplace_back("Value_" + padded);
            }
        }
    }
    return record;
}
