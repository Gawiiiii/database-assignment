#include "schema.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace {

std::string trim(const std::string& s) {
    std::size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) {
        ++first;
    }
    std::size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) {
        --last;
    }
    return s.substr(first, last - first);
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

FieldType parse_type(const std::string& token, int line_no) {
    const auto t = upper(token);
    if (t == "INT") {
        return FieldType::Int;
    }
    if (t == "CHAR") {
        return FieldType::Char;
    }
    if (t == "VARCHAR") {
        return FieldType::VarChar;
    }
    throw std::runtime_error("schema line " + std::to_string(line_no) + ": unknown field type '" + token + "'");
}

} // namespace

TableSchema TableSchema::load_from_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open schema file: " + path);
    }

    TableSchema schema;
    std::unordered_set<std::string> field_names;
    bool seen_table = false;
    bool seen_pk = false;

    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream iss(line);
        std::string keyword;
        iss >> keyword;
        keyword = upper(keyword);

        if (keyword == "TABLE") {
            if (seen_table) {
                throw std::runtime_error("schema line " + std::to_string(line_no) + ": duplicate TABLE declaration");
            }
            if (!(iss >> schema.table_name) || !trim(line.substr(line.find(schema.table_name) + schema.table_name.size())).empty()) {
                throw std::runtime_error("schema line " + std::to_string(line_no) + ": expected TABLE <name>");
            }
            seen_table = true;
        } else if (keyword == "PRIMARY_KEY") {
            if (seen_pk) {
                throw std::runtime_error("schema line " + std::to_string(line_no) + ": duplicate PRIMARY_KEY declaration");
            }
            if (!(iss >> schema.primary_key)) {
                throw std::runtime_error("schema line " + std::to_string(line_no) + ": expected PRIMARY_KEY <field>");
            }
            std::string extra;
            if (iss >> extra) {
                throw std::runtime_error("schema line " + std::to_string(line_no) + ": PRIMARY_KEY has extra token '" + extra + "'");
            }
            seen_pk = true;
        } else if (keyword == "FIELD") {
            Field field;
            std::string type_token;
            if (!(iss >> field.name >> type_token)) {
                throw std::runtime_error("schema line " + std::to_string(line_no) + ": expected FIELD <name> <type> [length]");
            }
            if (field_names.count(field.name)) {
                throw std::runtime_error("schema line " + std::to_string(line_no) + ": duplicate field '" + field.name + "'");
            }
            field.type = parse_type(type_token, line_no);
            if (field.type == FieldType::Int) {
                field.length = 4;
                std::string extra;
                if (iss >> extra) {
                    throw std::runtime_error("schema line " + std::to_string(line_no) + ": INT field should not have length");
                }
            } else {
                if (!(iss >> field.length) || field.length == 0) {
                    throw std::runtime_error("schema line " + std::to_string(line_no) + ": CHAR/VARCHAR field requires positive length");
                }
                std::string extra;
                if (iss >> extra) {
                    throw std::runtime_error("schema line " + std::to_string(line_no) + ": FIELD has extra token '" + extra + "'");
                }
            }
            field_names.insert(field.name);
            schema.fields.push_back(field);
        } else {
            throw std::runtime_error("schema line " + std::to_string(line_no) + ": unknown keyword '" + keyword + "'");
        }
    }

    if (!seen_table) {
        throw std::runtime_error("schema error: missing TABLE declaration");
    }
    if (!seen_pk) {
        throw std::runtime_error("schema error: missing PRIMARY_KEY declaration");
    }
    if (schema.fields.empty()) {
        throw std::runtime_error("schema error: no FIELD declarations");
    }
    if (schema.field_index(schema.primary_key) < 0) {
        throw std::runtime_error("schema error: PRIMARY_KEY '" + schema.primary_key + "' is not declared as a FIELD");
    }
    return schema;
}

int TableSchema::field_index(const std::string& name) const {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TableSchema::primary_key_index() const {
    return field_index(primary_key);
}

std::size_t TableSchema::fixed_area_size() const {
    std::size_t total = 0;
    for (const auto& field : fields) {
        if (field.type == FieldType::Int) {
            total += 4;
        } else if (field.type == FieldType::Char) {
            total += field.length;
        } else {
            total += 8;
        }
    }
    return total;
}

std::string field_type_name(FieldType type) {
    switch (type) {
        case FieldType::Int:
            return "INT";
        case FieldType::Char:
            return "CHAR";
        case FieldType::VarChar:
            return "VARCHAR";
    }
    return "UNKNOWN";
}
