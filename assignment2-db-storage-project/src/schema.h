#pragma once

#include <cstddef>
#include <string>
#include <vector>

enum class FieldType {
    Int,
    Char,
    VarChar
};

struct Field {
    std::string name;
    FieldType type;
    std::size_t length;
};

class TableSchema {
public:
    std::string table_name;
    std::string primary_key;
    std::vector<Field> fields;

    static TableSchema load_from_file(const std::string& path);
    int field_index(const std::string& name) const;
    int primary_key_index() const;
    std::size_t fixed_area_size() const;
};

std::string field_type_name(FieldType type);
