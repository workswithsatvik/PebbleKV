#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace pebble {

class SSTable;  // defined in pebble_kv.cpp

// Counts of SSTable probes during get(): a probe is "skipped" when the
// table's Bloom filter rules the key out, and "checked" otherwise.
struct BloomStats {
    std::uint64_t probes_checked = 0;
    std::uint64_t probes_skipped = 0;
};

// An educational LSM-style key-value store:
// WAL -> ordered memtable -> immutable SSTables (+ Bloom filters) -> compaction.
class PebbleKV {
public:
    static constexpr std::size_t kDefaultMemtableLimit = 16384;

    explicit PebbleKV(const std::filesystem::path& directory,
                      std::size_t memtable_limit = kDefaultMemtableLimit);
    ~PebbleKV();

    PebbleKV(const PebbleKV&) = delete;
    PebbleKV& operator=(const PebbleKV&) = delete;

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key) const;
    void flush();
    void compact();

    // Introspection used by tests and the benchmark.
    BloomStats bloom_stats() const;
    double bloom_skip_rate_percent() const;
    void reset_bloom_stats();
    std::size_t table_count() const;
    std::size_t memtable_size() const;

private:
    void load_tables();
    void replay_wal();
    void rewrite_wal();
    void flush_locked();
    std::filesystem::path table_path(std::uint64_t id) const;
    std::filesystem::path wal_path() const;

    std::filesystem::path dir_;
    std::size_t memtable_limit_;
    std::map<std::string, std::string> memtable_;
    std::vector<std::unique_ptr<SSTable>> tables_;  // oldest first
    std::uint64_t next_table_id_ = 1;
    std::ofstream wal_;

    mutable std::shared_mutex mutex_;  // shared: get; exclusive: put/flush/compact
    mutable std::atomic<std::uint64_t> probes_checked_{0};
    mutable std::atomic<std::uint64_t> probes_skipped_{0};
};

}  // namespace pebble
