// PebbleKV benchmark: 250k writes, reopen + recovery check, 1M point reads on
// eight threads (~half for missing keys), then compaction check.
// All numbers are measured with std::chrono::steady_clock; nothing is hard-coded.
#include "pebble_kv.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kWrites = 250'000;
constexpr std::uint64_t kReads = 1'000'000;
constexpr std::uint64_t kReaderThreads = 8;
constexpr std::uint64_t kSeed = 42;

// Stored keys use even ids and missing keys use odd ids, so missing keys fall
// inside the same key range as stored keys and must be ruled out by lookups.
std::string key_for(std::uint64_t id) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "key%010llu", static_cast<unsigned long long>(id));
    return buf;
}

std::string value_for(std::uint64_t id) {
    std::string v = "value-" + std::to_string(id) + "-";
    v.resize(64, 'x');
    return v;
}

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

bool has_expected(const pebble::PebbleKV& db, std::uint64_t id) {
    const auto v = db.get(key_for(id));
    return id % 2 == 0 ? (v && *v == value_for(id)) : !v.has_value();
}

}  // namespace

int main() {
    const fs::path dir = "pebblekv_benchmark_data";
    fs::remove_all(dir);

    // Deterministic insertion order: ids 0, 2, 4, ... shuffled with a fixed seed.
    std::vector<std::uint64_t> ids(kWrites);
    std::iota(ids.begin(), ids.end(), 0);
    for (auto& id : ids) id *= 2;
    std::mt19937_64 shuffle_rng(kSeed);
    std::shuffle(ids.begin(), ids.end(), shuffle_rng);

    // --- Write phase ---
    double write_seconds = 0;
    {
        pebble::PebbleKV db(dir);
        const auto start = Clock::now();
        for (std::uint64_t id : ids) db.put(key_for(id), value_for(id));
        write_seconds = seconds_since(start);
    }  // closed without a final flush: the newest writes exist only in the WAL

    // --- Reopen and verify every write (SSTables + WAL replay) ---
    pebble::PebbleKV db(dir);
    bool recovery_ok = true;
    for (std::uint64_t i = 0; i < kWrites && recovery_ok; ++i) {
        recovery_ok = has_expected(db, 2 * i) && has_expected(db, 2 * i + 1);
    }

    // --- Read phase: 1M lookups on 8 threads ---
    db.reset_bloom_stats();
    std::atomic<std::uint64_t> wrong_reads{0};
    std::vector<std::thread> readers;
    const auto read_start = Clock::now();
    for (std::uint64_t t = 0; t < kReaderThreads; ++t) {
        readers.emplace_back([&, t] {
            std::mt19937_64 rng(kSeed + 1 + t);
            std::uniform_int_distribution<std::uint64_t> pick(0, kWrites - 1);
            std::uint64_t wrong = 0;
            for (std::uint64_t i = 0; i < kReads / kReaderThreads; ++i) {
                const std::uint64_t id = 2 * pick(rng) + (rng() & 1);  // odd id => missing key
                if (!has_expected(db, id)) ++wrong;
            }
            wrong_reads.fetch_add(wrong);
        });
    }
    for (auto& r : readers) r.join();
    const double read_seconds = seconds_since(read_start);
    const double skip_rate = db.bloom_skip_rate_percent();

    // --- Compaction: merge all SSTables, then verify representative keys ---
    db.compact();
    bool compaction_ok = db.table_count() == 1;
    for (std::uint64_t id = 0; id < 2 * kWrites && compaction_ok; id += 997) {
        compaction_ok = has_expected(db, id);
    }

    std::printf("writes=%llu\n", static_cast<unsigned long long>(kWrites));
    std::printf("write_ops_per_second=%.0f\n", static_cast<double>(kWrites) / write_seconds);
    std::printf("reads=%llu\n", static_cast<unsigned long long>(kReads));
    std::printf("reader_threads=%llu\n", static_cast<unsigned long long>(kReaderThreads));
    std::printf("read_ops_per_second=%.0f\n", static_cast<double>(kReads) / read_seconds);
    std::printf("bloom_filter_skip_rate_percent=%.2f\n", skip_rate);
    std::printf("recovery_check=%s\n", recovery_ok ? "passed" : "failed");
    std::printf("compaction_check=%s\n", compaction_ok ? "passed" : "failed");

    if (wrong_reads.load() != 0) {
        std::fprintf(stderr, "error: %llu lookups returned wrong results\n",
                     static_cast<unsigned long long>(wrong_reads.load()));
    }
    return (recovery_ok && compaction_ok && wrong_reads.load() == 0) ? 0 : 1;
}
