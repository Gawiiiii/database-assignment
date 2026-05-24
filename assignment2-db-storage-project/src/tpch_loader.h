#pragma once

#include "record.h"
#include "schema.h"

#include <cstddef>
#include <functional>
#include <string>

bool load_tpch_orders(const std::string& tpch_dir,
                      const TableSchema& schema,
                      std::size_t limit,
                      const std::function<void(const Record&)>& consumer);

Record make_synthetic_record(const TableSchema& schema, std::size_t id);
