// Standalone tests. CHECK throws instead of using assert, which Release builds compile out.
#include "bloom_filter.hpp"
#include "pebble_kv.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using pebble::PebbleKV;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #cond); \
        }                                                                                  \
    } while (0)

namespace {

fs::path fresh_dir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("pebblekv_test_" + name);
    fs::remove_all(dir);
    return dir;
}

std::size_t count_sst_files(const fs::path& dir) {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) n += e.path().extension() == ".sst";
    return n;
}

std::string k(int i) { return "key" + std::to_string(i); }
std::string v(int i) { return "value" + std::to_string(i); }

void test_put_and_get(const fs::path& dir) {
    PebbleKV db(dir);
    db.put("apple", "red");
    db.put("banana", "yellow");
    CHECK(db.get("apple") == "red");
    CHECK(db.get("banana") == "yellow");
}

void test_missing_key(const fs::path& dir) {
    PebbleKV db(dir, 2);
    CHECK(!db.get("nothing").has_value());
    db.put("a", "1");
    db.put("c", "3");  // flushed to an SSTable
    CHECK(!db.get("b").has_value());   // between stored keys
    CHECK(!db.get("0").has_value());   // before the first key
    CHECK(!db.get("zzz").has_value()); // after the last key
}

void test_update_existing_key(const fs::path& dir) {
    PebbleKV db(dir);
    db.put("key", "old");
    db.put("key", "new");
    CHECK(db.get("key") == "new");
    CHECK(db.memtable_size() == 1);
}

void test_automatic_flush(const fs::path& dir) {
    PebbleKV db(dir, 4);
    for (int i = 0; i < 10; ++i) db.put(k(i), v(i));
    CHECK(db.table_count() == 2);  // flushed at 4 and 8 entries
    CHECK(db.memtable_size() == 2);
    CHECK(count_sst_files(dir) == 2);
    for (int i = 0; i < 10; ++i) CHECK(db.get(k(i)) == v(i));
}

void test_reopen(const fs::path& dir) {
    {
        PebbleKV db(dir, 3);
        for (int i = 0; i < 7; ++i) db.put(k(i), v(i));
    }
    PebbleKV db(dir, 3);
    CHECK(db.table_count() == 2);
    CHECK(db.memtable_size() == 1);
    for (int i = 0; i < 7; ++i) CHECK(db.get(k(i)) == v(i));
}

void test_wal_recovery_before_flush(const fs::path& dir) {
    {
        PebbleKV db(dir, 100);
        for (int i = 0; i < 5; ++i) db.put(k(i), v(i));
        db.put(k(2), "updated");
    }
    CHECK(count_sst_files(dir) == 0);  // everything lives in the WAL
    for (int round = 0; round < 2; ++round) {  // recovery must also survive a second reopen
        PebbleKV db(dir, 100);
        CHECK(db.memtable_size() == 5);
        CHECK(db.get(k(2)) == "updated");
        CHECK(db.get(k(4)) == v(4));
    }
}

void test_wal_torn_tail_is_ignored(const fs::path& dir) {
    {
        PebbleKV db(dir, 100);
        db.put("a", "1");
        db.put("b", "2");
    }
    {
        std::ofstream wal(dir / "wal.log", std::ios::binary | std::ios::app);
        wal.write("\x05\x00\x00", 3);  // simulate a crash mid-append
    }
    {
        PebbleKV db(dir, 100);
        CHECK(db.get("a") == "1");
        CHECK(db.get("b") == "2");
        db.put("c", "3");
    }
    PebbleKV db(dir, 100);
    CHECK(db.get("c") == "3");
    CHECK(db.memtable_size() == 3);
}

void test_multiple_sstables(const fs::path& dir) {
    PebbleKV db(dir, 2);
    db.put("a", "a1");
    db.put("b", "b1");  // table 1
    db.put("c", "c1");
    db.put("a", "a2");  // table 2
    db.put("d", "d1");
    db.put("b", "b2");  // table 3
    CHECK(db.table_count() == 3);
    CHECK(db.get("a") == "a2");  // newer table wins
    CHECK(db.get("b") == "b2");
    CHECK(db.get("c") == "c1");
    CHECK(db.get("d") == "d1");
    CHECK(!db.get("e").has_value());
}

void test_compaction_keeps_newest(const fs::path& dir) {
    {
        PebbleKV db(dir, 2);
        db.put("a", "a1");
        db.put("b", "b1");
        db.put("a", "a2");
        db.put("c", "c1");
        db.put("a", "a3");
        db.put("b", "b2");
        CHECK(db.table_count() == 3);
        db.compact();
        CHECK(db.table_count() == 1);
        CHECK(count_sst_files(dir) == 1);
        CHECK(db.get("a") == "a3");
        CHECK(db.get("b") == "b2");
        CHECK(db.get("c") == "c1");
        db.put("a", "a4");
        db.flush();
        CHECK(db.get("a") == "a4");  // tables flushed after compaction are newer
    }
    PebbleKV db(dir, 2);
    CHECK(db.get("a") == "a4");
    CHECK(db.get("b") == "b2");
}

void test_concurrent_readers(const fs::path& dir) {
    PebbleKV db(dir, 256);
    constexpr int kKeys = 3000;
    for (int i = 0; i < kKeys; ++i) db.put(k(i), v(i));
    db.flush();

    std::atomic<int> errors{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&] {
            for (int i = 0; i < kKeys; ++i) {
                if (db.get(k(i)) != v(i)) ++errors;
                if (db.get("missing" + std::to_string(i)).has_value()) ++errors;
            }
        });
    }
    for (auto& r : readers) r.join();
    CHECK(errors == 0);
    const auto stats = db.bloom_stats();
    CHECK(stats.probes_skipped > 0);
    CHECK(db.bloom_skip_rate_percent() > 0.0 && db.bloom_skip_rate_percent() <= 100.0);
}

void test_concurrent_reads_and_writes(const fs::path& dir) {
    PebbleKV db(dir, 128);
    constexpr int kStable = 500;
    constexpr int kWrites = 3000;
    for (int i = 0; i < kStable; ++i) db.put("stable" + std::to_string(i), v(i));

    std::atomic<int> errors{0};
    std::thread writer([&] {
        for (int i = 0; i < kWrites; ++i) {
            db.put(k(i), v(i));
            db.put("hot", "hot-" + std::to_string(i));
            if (i % 1000 == 999) db.compact();
        }
    });
    // Readers run a fixed number of iterations rather than "until the writer is done":
    // std::shared_mutex may prefer readers (glibc does), so nonstop readers can starve writers.
    std::vector<std::thread> readers;
    for (int t = 0; t < 6; ++t) {
        readers.emplace_back([&, t] {
            for (int i = 0; i < 5000; ++i) {
                const int s = (i * 7 + t) % kStable;
                if (db.get("stable" + std::to_string(s)) != v(s)) ++errors;
                const auto w = db.get(k(i % kWrites));  // may not be written yet
                if (w && *w != v(i % kWrites)) ++errors;
                const auto h = db.get("hot");
                if (h && h->rfind("hot-", 0) != 0) ++errors;
            }
        });
    }
    writer.join();
    for (auto& r : readers) r.join();

    CHECK(errors == 0);
    for (int i = 0; i < kWrites; ++i) CHECK(db.get(k(i)) == v(i));
    CHECK(db.get("hot") == "hot-" + std::to_string(kWrites - 1));
}

void test_bloom_filter(const fs::path&) {
    pebble::BloomFilter bloom(1000);
    for (int i = 0; i < 1000; ++i) bloom.add(k(i));
    for (int i = 0; i < 1000; ++i) CHECK(bloom.may_contain(k(i)));  // no false negatives
    int false_positives = 0;
    for (int i = 1000; i < 11000; ++i) false_positives += bloom.may_contain(k(i));
    CHECK(false_positives < 500);  // well under 5% at 10 bits per key
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void(const fs::path&)>>> tests = {
        {"put_and_get", test_put_and_get},
        {"missing_key", test_missing_key},
        {"update_existing_key", test_update_existing_key},
        {"automatic_flush", test_automatic_flush},
        {"reopen", test_reopen},
        {"wal_recovery_before_flush", test_wal_recovery_before_flush},
        {"wal_torn_tail_is_ignored", test_wal_torn_tail_is_ignored},
        {"multiple_sstables", test_multiple_sstables},
        {"compaction_keeps_newest", test_compaction_keeps_newest},
        {"concurrent_readers", test_concurrent_readers},
        {"concurrent_reads_and_writes", test_concurrent_reads_and_writes},
        {"bloom_filter", test_bloom_filter},
    };

    int failures = 0;
    for (const auto& [name, fn] : tests) {
        const fs::path dir = fresh_dir(name);
        try {
            fn(dir);
            std::cout << "[PASS] " << name << "\n";
        } catch (const std::exception& e) {
            ++failures;
            std::cout << "[FAIL] " << name << ": " << e.what() << "\n";
        }
        fs::remove_all(dir);
    }
    std::cout << (tests.size() - failures) << "/" << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
