#pragma once

#include "schema.h"

#include <cstddef>
#include <string>

struct BenchmarkOptions {
    std::string schema_path = "schema/student.schema.txt";
    std::string dataset = "synthetic";
    std::string tpch_dir = ".";
    std::size_t limit = 1000000;
    std::string benchmark = "all";
    std::string output = "results/benchmark_result.csv";
};

int run_benchmark(const BenchmarkOptions& options);
