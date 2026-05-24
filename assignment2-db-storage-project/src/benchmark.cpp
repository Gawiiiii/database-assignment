#include "benchmark.h"

#include "record.h"
#include "storage_engine.h"
#include "tpch_loader.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <sys/resource.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct CsvRow {
    double timestamp_sec = 0.0;
    std::string phase;
    uint64_t ops = 0;
    uint64_t total_ops = 0;
    double tps = 0.0;
    double logical_read_MBps = 0.0;
    double logical_write_MBps = 0.0;
    double avg_latency_us = 0.0;
    double p50_latency_us = 0.0;
    double p95_latency_us = 0.0;
    double p99_latency_us = 0.0;
    std::size_t total_pages = 0;
    double memory_usage_MB = 0.0;
};

class CsvWriter {
public:
    explicit CsvWriter(const std::string& path) : out_(path) {
        if (!out_) {
            throw std::runtime_error("cannot open benchmark output: " + path);
        }
        out_ << "timestamp_sec,phase,ops,total_ops,tps,logical_read_MBps,logical_write_MBps,"
                "avg_latency_us,p50_latency_us,p95_latency_us,p99_latency_us,total_pages,memory_usage_MB\n";
    }

    void write(const CsvRow& row) {
        out_ << row.timestamp_sec << ','
             << row.phase << ','
             << row.ops << ','
             << row.total_ops << ','
             << row.tps << ','
             << row.logical_read_MBps << ','
             << row.logical_write_MBps << ','
             << row.avg_latency_us << ','
             << row.p50_latency_us << ','
             << row.p95_latency_us << ','
             << row.p99_latency_us << ','
             << row.total_pages << ','
             << row.memory_usage_MB << '\n';
    }

private:
    std::ofstream out_;
};

double seconds_since(const Clock::time_point& start, const Clock::time_point& end = Clock::now()) {
    return std::chrono::duration<double>(end - start).count();
}

double memory_usage_mb() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
#ifdef __APPLE__
    return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
}

double bytes_to_mbps(uint64_t bytes, double sec) {
    if (sec <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(bytes) / (1024.0 * 1024.0) / sec;
}

double percentile_us(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t idx = static_cast<std::size_t>(std::min<double>(values.size() - 1, std::ceil(p * values.size()) - 1));
    return values[idx];
}

std::size_t sample_ops_for(std::size_t total_ops) {
    return std::max<std::size_t>(1, total_ops / 20);
}

Record make_updated_record(const TableSchema& schema, Record record, std::size_t seq) {
    for (std::size_t i = 0; i < schema.fields.size(); ++i) {
        if (schema.fields[i].type == FieldType::VarChar) {
            const std::string value = (seq % 2 == 0)
                ? ("U_" + std::to_string(seq))
                : ("Updated_variable_length_value_" + std::to_string(seq) + "_payload");
            record.values[i] = value.substr(0, schema.fields[i].length);
            return record;
        }
    }
    for (std::size_t i = 0; i < schema.fields.size(); ++i) {
        if (schema.fields[i].type == FieldType::Int && static_cast<int>(i) != schema.primary_key_index()) {
            auto v = std::get<int32_t>(record.values[i]);
            record.values[i] = static_cast<int32_t>(v + 1);
            return record;
        }
    }
    return record;
}

class BenchmarkRunner {
public:
    BenchmarkRunner(TableSchema schema, const BenchmarkOptions& options, CsvWriter& writer)
        : schema_(std::move(schema)), options_(options), writer_(writer), engine_(schema_) {}

    void run() {
        insert_phase();
        if (options_.benchmark == "insert") {
            return;
        }
        if (options_.benchmark == "all" || options_.benchmark == "delete") {
            delete_phase();
        }
        if (options_.benchmark == "all" || options_.benchmark == "update") {
            update_phase();
        }
        if (options_.benchmark == "all" || options_.benchmark == "query") {
            query_phase();
        }
    }

private:
    void insert_record(const Record& record,
                       const Clock::time_point& phase_start,
                       Clock::time_point& interval_start,
                       uint64_t& interval_ops,
                       uint64_t& total_ops,
                       uint64_t& prev_read,
                       uint64_t& prev_write,
                       std::size_t sample_ops) {
        engine_.insert(record);
        keys_.push_back(RecordCodec::primary_key_to_string(schema_, record));
        alive_.push_back(true);
        ++interval_ops;
        ++total_ops;
        const auto now = Clock::now();
        if (interval_ops >= sample_ops || seconds_since(interval_start, now) >= 1.0) {
            emit_throughput_row("insert", phase_start, interval_start, now, interval_ops, total_ops, prev_read, prev_write);
            interval_start = now;
            interval_ops = 0;
        }
    }

    void insert_phase() {
        std::cout << "insert benchmark: dataset=" << options_.dataset << ", limit=" << options_.limit << '\n';
        const auto phase_start = Clock::now();
        auto interval_start = phase_start;
        uint64_t interval_ops = 0;
        uint64_t total_ops = 0;
        uint64_t prev_read = engine_.io_stats().logical_read_bytes;
        uint64_t prev_write = engine_.io_stats().logical_write_bytes;
        const std::size_t sample_ops = sample_ops_for(options_.limit);

        bool loaded_tpch = false;
        if (options_.dataset == "tpch") {
            loaded_tpch = load_tpch_orders(options_.tpch_dir, schema_, options_.limit, [&](const Record& record) {
                insert_record(record, phase_start, interval_start, interval_ops, total_ops, prev_read, prev_write, sample_ops);
            });
            if (!loaded_tpch) {
                std::cerr << "orders.tbl not found or empty under " << options_.tpch_dir
                          << "; falling back to synthetic records matching the selected schema\n";
            }
        }

        if (options_.dataset != "tpch" || !loaded_tpch) {
            for (std::size_t i = 1; i <= options_.limit; ++i) {
                insert_record(make_synthetic_record(schema_, i), phase_start, interval_start, interval_ops, total_ops, prev_read, prev_write, sample_ops);
            }
        }

        if (interval_ops > 0 || total_ops == 0) {
            emit_throughput_row("insert", phase_start, interval_start, Clock::now(), interval_ops, total_ops, prev_read, prev_write);
        }
    }

    void delete_phase() {
        auto indices = shuffled_alive_indices();
        const std::size_t op_limit = std::min<std::size_t>(100000, indices.size() / 10);
        std::cout << "delete benchmark: ops=" << op_limit << '\n';
        run_simple_phase("delete", op_limit, [&](std::size_t i) {
            const std::size_t idx = indices[i];
            if (engine_.erase_by_key(keys_[idx])) {
                alive_[idx] = false;
                return true;
            }
            return false;
        });
    }

    void update_phase() {
        auto indices = shuffled_alive_indices();
        const std::size_t op_limit = std::min<std::size_t>(100000, indices.size());
        std::cout << "update benchmark: ops=" << op_limit << '\n';
        run_simple_phase("update", op_limit, [&](std::size_t i) {
            const std::string& key = keys_[indices[i]];
            auto old = engine_.read_by_key(key);
            if (!old) {
                return false;
            }
            return engine_.update_by_key(key, make_updated_record(schema_, *old, i));
        });
    }

    void query_phase() {
        auto indices = shuffled_alive_indices();
        const std::size_t op_limit = std::min<std::size_t>(100000, indices.size());
        std::cout << "query benchmark: ops=" << op_limit << '\n';
        if (op_limit == 0) {
            return;
        }

        const auto phase_start = Clock::now();
        auto interval_start = phase_start;
        uint64_t interval_ops = 0;
        uint64_t total_ops = 0;
        uint64_t prev_read = engine_.io_stats().logical_read_bytes;
        uint64_t prev_write = engine_.io_stats().logical_write_bytes;
        std::vector<double> latencies;
        const std::size_t sample_ops = sample_ops_for(op_limit);
        latencies.reserve(sample_ops);

        for (std::size_t i = 0; i < op_limit; ++i) {
            const auto q_start = Clock::now();
            volatile bool found = engine_.read_by_key(keys_[indices[i]]).has_value();
            (void)found;
            const auto q_end = Clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(q_end - q_start).count());
            ++interval_ops;
            ++total_ops;

            const auto now = Clock::now();
            if (interval_ops >= sample_ops || seconds_since(interval_start, now) >= 1.0) {
                emit_latency_row("query", phase_start, interval_start, now, interval_ops, total_ops, prev_read, prev_write, latencies);
                interval_start = now;
                interval_ops = 0;
                latencies.clear();
            }
        }
        if (interval_ops > 0) {
            emit_latency_row("query", phase_start, interval_start, Clock::now(), interval_ops, total_ops, prev_read, prev_write, latencies);
        }
    }

    template <typename Fn>
    void run_simple_phase(const std::string& phase, std::size_t op_limit, Fn fn) {
        const auto phase_start = Clock::now();
        auto interval_start = phase_start;
        uint64_t interval_ops = 0;
        uint64_t total_ops = 0;
        uint64_t prev_read = engine_.io_stats().logical_read_bytes;
        uint64_t prev_write = engine_.io_stats().logical_write_bytes;
        const std::size_t sample_ops = sample_ops_for(op_limit);

        for (std::size_t i = 0; i < op_limit; ++i) {
            if (fn(i)) {
                ++interval_ops;
                ++total_ops;
            }
            const auto now = Clock::now();
            if (interval_ops >= sample_ops || seconds_since(interval_start, now) >= 1.0) {
                emit_throughput_row(phase, phase_start, interval_start, now, interval_ops, total_ops, prev_read, prev_write);
                interval_start = now;
                interval_ops = 0;
            }
        }
        if (interval_ops > 0 || op_limit == 0) {
            emit_throughput_row(phase, phase_start, interval_start, Clock::now(), interval_ops, total_ops, prev_read, prev_write);
        }
    }

    std::vector<std::size_t> shuffled_alive_indices() const {
        std::vector<std::size_t> indices;
        indices.reserve(keys_.size());
        for (std::size_t i = 0; i < alive_.size(); ++i) {
            if (alive_[i]) {
                indices.push_back(i);
            }
        }
        std::mt19937_64 rng(20260521);
        std::shuffle(indices.begin(), indices.end(), rng);
        return indices;
    }

    void emit_throughput_row(const std::string& phase,
                             const Clock::time_point& phase_start,
                             const Clock::time_point& interval_start,
                             const Clock::time_point& now,
                             uint64_t ops,
                             uint64_t total_ops,
                             uint64_t& prev_read,
                             uint64_t& prev_write) {
        const double interval_sec = std::max(1e-9, seconds_since(interval_start, now));
        const uint64_t cur_read = engine_.io_stats().logical_read_bytes;
        const uint64_t cur_write = engine_.io_stats().logical_write_bytes;
        CsvRow row;
        row.timestamp_sec = seconds_since(phase_start, now);
        row.phase = phase;
        row.ops = ops;
        row.total_ops = total_ops;
        row.tps = static_cast<double>(ops) / interval_sec;
        row.logical_read_MBps = bytes_to_mbps(cur_read - prev_read, interval_sec);
        row.logical_write_MBps = bytes_to_mbps(cur_write - prev_write, interval_sec);
        row.total_pages = engine_.page_count();
        row.memory_usage_MB = memory_usage_mb();
        writer_.write(row);
        prev_read = cur_read;
        prev_write = cur_write;
    }

    void emit_latency_row(const std::string& phase,
                          const Clock::time_point& phase_start,
                          const Clock::time_point& interval_start,
                          const Clock::time_point& now,
                          uint64_t ops,
                          uint64_t total_ops,
                          uint64_t& prev_read,
                          uint64_t& prev_write,
                          const std::vector<double>& latencies) {
        const double interval_sec = std::max(1e-9, seconds_since(interval_start, now));
        const uint64_t cur_read = engine_.io_stats().logical_read_bytes;
        const uint64_t cur_write = engine_.io_stats().logical_write_bytes;
        CsvRow row;
        row.timestamp_sec = seconds_since(phase_start, now);
        row.phase = phase;
        row.ops = ops;
        row.total_ops = total_ops;
        row.tps = static_cast<double>(ops) / interval_sec;
        row.logical_read_MBps = bytes_to_mbps(cur_read - prev_read, interval_sec);
        row.logical_write_MBps = bytes_to_mbps(cur_write - prev_write, interval_sec);
        if (!latencies.empty()) {
            row.avg_latency_us = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
            row.p50_latency_us = percentile_us(latencies, 0.50);
            row.p95_latency_us = percentile_us(latencies, 0.95);
            row.p99_latency_us = percentile_us(latencies, 0.99);
        }
        row.total_pages = engine_.page_count();
        row.memory_usage_MB = memory_usage_mb();
        writer_.write(row);
        prev_read = cur_read;
        prev_write = cur_write;
    }

    TableSchema schema_;
    BenchmarkOptions options_;
    CsvWriter& writer_;
    StorageEngine engine_;
    std::vector<std::string> keys_;
    std::vector<bool> alive_;
};

} // namespace

int run_benchmark(const BenchmarkOptions& options) {
    std::filesystem::create_directories(std::filesystem::path(options.output).parent_path());
    TableSchema schema = TableSchema::load_from_file(options.schema_path);
    CsvWriter writer(options.output);
    BenchmarkRunner runner(std::move(schema), options, writer);
    runner.run();
    return 0;
}
