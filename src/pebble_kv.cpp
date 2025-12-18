#include "pebble_kv.hpp"

#include "bloom_filter.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace pebble {
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kTableMagic = 0x50424C31;  // "PBL1"
constexpr std::size_t kIndexInterval = 16;         // one sparse-index entry per 16 records
constexpr std::size_t kMaxFieldSize = std::numeric_limits<std::uint32_t>::max();

// Integers are stored in native byte order (little-endian on x86/ARM).
void write_u32(std::ostream& out, std::uint32_t v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof v);
}
void write_u64(std::ostream& out, std::uint64_t v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof v);
}
bool read_u32(std::istream& in, std::uint32_t& v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof v));
}
bool read_u64(std::istream& in, std::uint64_t& v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof v));
}
std::uint32_t load_u32(const char* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// Record layout, shared by the WAL and SSTables: [u32 klen][u32 vlen][key][value]
void write_record(std::ostream& out, const std::string& key, const std::string& value) {
    write_u32(out, static_cast<std::uint32_t>(key.size()));
    write_u32(out, static_cast<std::uint32_t>(value.size()));
    out.write(key.data(), static_cast<std::streamsize>(key.size()));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

// Reads one record; returns false at end of stream or on a truncated record.
bool read_record(std::istream& in, std::string& key, std::string& value) {
    std::uint32_t klen = 0;
    std::uint32_t vlen = 0;
    if (!read_u32(in, klen) || !read_u32(in, vlen)) return false;
    key.resize(klen);
    value.resize(vlen);
    return in.read(key.data(), klen) && in.read(value.data(), vlen);
}

}  // namespace

// ---------------------------------------------------------------------------
// SSTable: an immutable, sorted file. Only a sparse index and a Bloom filter
// are kept in memory; values are read from disk on demand.
// File layout: [u32 magic][u64 count][record]...
// ---------------------------------------------------------------------------
class SSTable {
public:
    // Writes a table via temp file + rename so a crash never leaves a half-written .sst.
    static void write(const fs::path& path, const std::map<std::string, std::string>& data) {
        const fs::path tmp = path.string() + ".tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        write_u32(out, kTableMagic);
        write_u64(out, data.size());
        for (const auto& [key, value] : data) write_record(out, key, value);
        out.close();
        if (!out) throw std::runtime_error("failed to write SSTable " + tmp.string());
        fs::rename(tmp, path);
    }

    explicit SSTable(fs::path path) : path_(std::move(path)) {
        file_.open(path_, std::ios::binary);
        std::uint32_t magic = 0;
        std::uint64_t count = 0;
        if (!read_u32(file_, magic) || magic != kTableMagic || !read_u64(file_, count)) {
            throw std::runtime_error("bad SSTable header: " + path_.string());
        }
        // One pass over the file builds the sparse index and the Bloom filter.
        bloom_ = BloomFilter(static_cast<std::size_t>(count));
        std::string key;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto offset = static_cast<std::uint64_t>(file_.tellg());
            std::uint32_t klen = 0;
            std::uint32_t vlen = 0;
            if (!read_u32(file_, klen) || !read_u32(file_, vlen)) break;
            key.resize(klen);
            if (!file_.read(key.data(), klen)) break;
            file_.seekg(vlen, std::ios::cur);
            bloom_.add(key);
            if (i % kIndexInterval == 0) index_.push_back({key, offset});
        }
        data_end_ = file_ ? static_cast<std::uint64_t>(file_.tellg()) : 0;
        if (!file_ || data_end_ != fs::file_size(path_)) {
            throw std::runtime_error("corrupt SSTable: " + path_.string());
        }
    }

    const fs::path& path() const { return path_; }
    bool may_contain(const std::string& key) const { return bloom_.may_contain(key); }

    // Binary search the sparse index, then scan the one block that could hold the key.
    std::optional<std::string> get(const std::string& key) const {
        auto it = std::upper_bound(index_.begin(), index_.end(), key,
                                   [](const std::string& k, const IndexEntry& e) { return k < e.key; });
        if (it == index_.begin()) return std::nullopt;  // key sorts before the first record
        const std::uint64_t block_end = (it == index_.end()) ? data_end_ : it->offset;
        --it;

        std::string block(static_cast<std::size_t>(block_end - it->offset), '\0');
        {
            // One stream per table is shared by all readers, so reads are serialized here.
            std::lock_guard<std::mutex> lock(file_mutex_);
            file_.clear();
            file_.seekg(static_cast<std::streamoff>(it->offset));
            if (!file_.read(block.data(), static_cast<std::streamsize>(block.size()))) {
                throw std::runtime_error("failed to read SSTable block: " + path_.string());
            }
        }

        std::size_t pos = 0;
        while (pos + 8 <= block.size()) {
            const std::size_t klen = load_u32(block.data() + pos);
            const std::size_t vlen = load_u32(block.data() + pos + 4);
            pos += 8;
            if (pos + klen + vlen > block.size()) break;
            const std::string_view k(block.data() + pos, klen);
            if (k == key) return std::string(block.data() + pos + klen, vlen);
            if (k > key) break;  // records are sorted: the key is not here
            pos += klen + vlen;
        }
        return std::nullopt;
    }

    // Sequential scan of every record (used by compaction) on a private stream.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        std::ifstream in(path_, std::ios::binary);
        std::uint32_t magic = 0;
        std::uint64_t count = 0;
        if (!read_u32(in, magic) || !read_u64(in, count)) {
            throw std::runtime_error("bad SSTable header: " + path_.string());
        }
        std::string key;
        std::string value;
        for (std::uint64_t i = 0; i < count; ++i) {
            if (!read_record(in, key, value)) throw std::runtime_error("corrupt SSTable: " + path_.string());
            fn(key, value);
        }
    }

private:
    struct IndexEntry {
        std::string key;
        std::uint64_t offset;
    };

    fs::path path_;
    std::vector<IndexEntry> index_;
    std::uint64_t data_end_ = 0;
    BloomFilter bloom_;
    mutable std::mutex file_mutex_;
    mutable std::ifstream file_;
};

// ---------------------------------------------------------------------------
// PebbleKV
// ---------------------------------------------------------------------------
PebbleKV::PebbleKV(const fs::path& directory, std::size_t memtable_limit)
    : dir_(directory), memtable_limit_(std::max<std::size_t>(1, memtable_limit)) {
    fs::create_directories(dir_);
    load_tables();
    replay_wal();
    rewrite_wal();
}

PebbleKV::~PebbleKV() = default;

fs::path PebbleKV::table_path(std::uint64_t id) const {
    char name[32];
    std::snprintf(name, sizeof name, "sst_%06llu.sst", static_cast<unsigned long long>(id));
    return dir_ / name;
}

fs::path PebbleKV::wal_path() const { return dir_ / "wal.log"; }

void PebbleKV::load_tables() {
    std::vector<std::uint64_t> ids;
    std::vector<fs::path> leftovers;
    for (const auto& entry : fs::directory_iterator(dir_)) {
        const fs::path& p = entry.path();
        const std::string name = p.filename().string();
        if (p.extension() == ".tmp") {
            leftovers.push_back(p);  // unfinished flush/compaction from a crash
        } else if (p.extension() == ".sst" && name.rfind("sst_", 0) == 0) {
            ids.push_back(std::stoull(name.substr(4, name.size() - 8)));
        }
    }
    for (const auto& p : leftovers) fs::remove(p);

    std::sort(ids.begin(), ids.end());  // file id order == age order
    for (std::uint64_t id : ids) {
        tables_.push_back(std::make_unique<SSTable>(table_path(id)));
        next_table_id_ = id + 1;
    }
}

void PebbleKV::replay_wal() {
    std::ifstream in(wal_path(), std::ios::binary);
    std::string key;
    std::string value;
    // Stops at end of file or at a torn record left by a crash mid-append.
    while (in && read_record(in, key, value)) memtable_[key] = value;
}

// Rewrites the log from the recovered memtable (temp file + rename), so a torn
// tail is dropped instead of sitting in front of newly appended records.
void PebbleKV::rewrite_wal() {
    const fs::path tmp = dir_ / "wal.log.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (const auto& [key, value] : memtable_) write_record(out, key, value);
        out.close();
        if (!out) throw std::runtime_error("failed to rewrite WAL");
    }
    fs::rename(tmp, wal_path());
    wal_.open(wal_path(), std::ios::binary | std::ios::app);
    if (!wal_) throw std::runtime_error("failed to open WAL");
}

void PebbleKV::put(const std::string& key, const std::string& value) {
    if (key.size() > kMaxFieldSize || value.size() > kMaxFieldSize) {
        throw std::invalid_argument("key or value too large");
    }
    std::unique_lock lock(mutex_);
    write_record(wal_, key, value);
    wal_.flush();  // hands the record to the OS; no fsync (see README limitations)
    if (!wal_) throw std::runtime_error("WAL append failed");
    memtable_[key] = value;
    if (memtable_.size() >= memtable_limit_) flush_locked();
}

std::optional<std::string> PebbleKV::get(const std::string& key) const {
    std::shared_lock lock(mutex_);
    if (auto it = memtable_.find(key); it != memtable_.end()) return it->second;

    std::uint64_t checked = 0;
    std::uint64_t skipped = 0;
    std::optional<std::string> result;
    for (auto it = tables_.rbegin(); it != tables_.rend(); ++it) {  // newest first
        if (!(*it)->may_contain(key)) {
            ++skipped;
            continue;
        }
        ++checked;
        result = (*it)->get(key);
        if (result) break;
    }
    // Counters are updated once per lookup to limit contention between readers.
    if (checked) probes_checked_.fetch_add(checked, std::memory_order_relaxed);
    if (skipped) probes_skipped_.fetch_add(skipped, std::memory_order_relaxed);
    return result;
}

void PebbleKV::flush() {
    std::unique_lock lock(mutex_);
    flush_locked();
}

void PebbleKV::flush_locked() {
    if (memtable_.empty()) return;
    const fs::path path = table_path(next_table_id_++);
    SSTable::write(path, memtable_);
    tables_.push_back(std::make_unique<SSTable>(path));
    memtable_.clear();

    // The data is now in an SSTable, so the log can start over.
    wal_.close();
    wal_.open(wal_path(), std::ios::binary | std::ios::trunc);
    if (!wal_) throw std::runtime_error("failed to reset WAL");
}

void PebbleKV::compact() {
    std::unique_lock lock(mutex_);
    if (tables_.size() < 2) return;

    // Oldest to newest, so newer values overwrite older ones.
    std::map<std::string, std::string> merged;
    for (const auto& table : tables_) {
        table->for_each([&](const std::string& k, const std::string& v) { merged.insert_or_assign(k, v); });
    }

    const fs::path path = table_path(next_table_id_++);
    SSTable::write(path, merged);  // throws before anything is deleted if the write fails
    auto compacted = std::make_unique<SSTable>(path);

    std::vector<fs::path> obsolete;
    for (const auto& table : tables_) obsolete.push_back(table->path());
    tables_.clear();
    tables_.push_back(std::move(compacted));
    for (const auto& p : obsolete) fs::remove(p);
}

BloomStats PebbleKV::bloom_stats() const {
    return {probes_checked_.load(std::memory_order_relaxed), probes_skipped_.load(std::memory_order_relaxed)};
}

double PebbleKV::bloom_skip_rate_percent() const {
    const BloomStats s = bloom_stats();
    const std::uint64_t total = s.probes_checked + s.probes_skipped;
    return total == 0 ? 0.0 : 100.0 * static_cast<double>(s.probes_skipped) / static_cast<double>(total);
}

void PebbleKV::reset_bloom_stats() {
    probes_checked_.store(0, std::memory_order_relaxed);
    probes_skipped_.store(0, std::memory_order_relaxed);
}

std::size_t PebbleKV::table_count() const {
    std::shared_lock lock(mutex_);
    return tables_.size();
}

std::size_t PebbleKV::memtable_size() const {
    std::shared_lock lock(mutex_);
    return memtable_.size();
}

}  // namespace pebble
