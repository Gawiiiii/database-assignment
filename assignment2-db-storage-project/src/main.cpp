#include "benchmark.h"
#include "record.h"
#include "schema.h"
#include "storage_engine.h"
#include "tpch_loader.h"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

struct CommandLineOptions : BenchmarkOptions {
    std::optional<std::string> query_key;
};

void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " --schema <schema.txt> --dataset synthetic|tpch "
        << "[--tpch-dir <dir>] [--limit N] [--benchmark all|insert|delete|update|query] "
        << "[--output results/benchmark_result.csv]\n"
        << "       " << argv0 << " --schema <schema.txt> --dataset synthetic|tpch "
        << "[--tpch-dir <dir>] [--limit N] --query-key <primary-key>\n";
}

CommandLineOptions parse_args(int argc, char** argv) {
    CommandLineOptions opt;
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
        } else if (arg == "--query-key") {
            opt.query_key = need_value(arg);
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

std::string value_to_string(const FieldValue& value) {
    if (auto p = std::get_if<int32_t>(&value)) {
        return std::to_string(*p);
    }
    return std::get<std::string>(value);
}

void print_record(const TableSchema& schema, const Record& record) {
    for (std::size_t i = 0; i < schema.fields.size(); ++i) {
        if (i > 0) {
            std::cout << '|';
        }
        std::cout << schema.fields[i].name << '=' << value_to_string(record.values[i]);
    }
    std::cout << '\n';
}

int run_single_query(const CommandLineOptions& options) {
    TableSchema schema = TableSchema::load_from_file(options.schema_path);
    StorageEngine engine(schema);

    bool loaded_tpch = false;
    if (options.dataset == "tpch") {
        loaded_tpch = load_tpch_orders(options.tpch_dir, schema, options.limit, [&](const Record& record) {
            engine.insert(record);
        });
        if (!loaded_tpch) {
            std::cerr << "orders.tbl not found or empty under " << options.tpch_dir
                      << "; falling back to synthetic records matching the selected schema\n";
        }
    }

    if (options.dataset != "tpch" || !loaded_tpch) {
        for (std::size_t i = 1; i <= options.limit; ++i) {
            engine.insert(make_synthetic_record(schema, i));
        }
    }

    const auto record = engine.read_by_key(*options.query_key);
    if (!record) {
        std::cout << "NOT_FOUND\n";
        return 2;
    }
    print_record(schema, *record);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_args(argc, argv);
        if (options.query_key) {
            return run_single_query(options);
        }
        return run_benchmark(options);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
