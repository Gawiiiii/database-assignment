#include "benchmark.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " --schema <schema.txt> --dataset synthetic|tpch "
        << "[--tpch-dir <dir>] [--limit N] [--benchmark all|insert|delete|update|query] "
        << "[--output results/benchmark_result.csv]\n";
}

BenchmarkOptions parse_args(int argc, char** argv) {
    BenchmarkOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + name);
            }
            return argv[++i];
        };
        if (arg == "--schema") {
            opt.schema_path = need_value(arg);
        } else if (arg == "--dataset") {
            opt.dataset = need_value(arg);
        } else if (arg == "--tpch-dir") {
            opt.tpch_dir = need_value(arg);
        } else if (arg == "--limit") {
            opt.limit = static_cast<std::size_t>(std::stoull(need_value(arg)));
        } else if (arg == "--benchmark") {
            opt.benchmark = need_value(arg);
        } else if (arg == "--output") {
            opt.output = need_value(arg);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    if (opt.dataset != "synthetic" && opt.dataset != "tpch") {
        throw std::runtime_error("--dataset must be synthetic or tpch");
    }
    if (opt.benchmark != "all" && opt.benchmark != "insert" &&
        opt.benchmark != "delete" && opt.benchmark != "update" && opt.benchmark != "query") {
        throw std::runtime_error("--benchmark must be all, insert, delete, update, or query");
    }
    return opt;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run_benchmark(parse_args(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
