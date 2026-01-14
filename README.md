# PebbleKV: Concurrent LSM-Style Storage Engine

PebbleKV is a small, educational key-value storage engine written in C++20 using only the standard library. It follows the log-structured merge (LSM) design used by engines like LevelDB and RocksDB, reduced to its core ideas. Writes go to a write-ahead log and an ordered in-memory table. Full memtables are flushed to immutable sorted files (SSTables), each with a Bloom filter that lets point lookups skip tables that cannot contain a key, and compaction merges tables back together. The whole engine is under 1,000 lines so every part can be read and explained. **It is a learning project, not a production database.**

## Features

- **Write-ahead log (WAL).** Every write is appended to `wal.log` before the memtable is updated, and the log is replayed when the database reopens.
- **Ordered memtable.** A `std::map<std::string, std::string>` that flushes automatically after a configurable number of entries (default 16,384).
- **Immutable SSTables.** Sorted binary files. Opening a table loads a sparse in-memory index (one key per 16 records), and lookups binary-search that index and then read a single block from disk.
- **Bloom filters.** One per SSTable, using a bit vector, 10 bits per key, and 3 hash probes. Tables whose filter rules a key out are skipped, and the engine counts checked and skipped probes.
- **Compaction.** Merges all SSTables into one, keeps the newest value for each key, and deletes old files only after the merged table is safely written.
- **Concurrency.** `std::shared_mutex` lets `get` calls run in parallel, while `put`, `flush`, and `compact` take an exclusive lock.
- **Tests.** Twelve standalone tests, including concurrency tests that pass under ThreadSanitizer.
- **Benchmark.** 250,000 writes, a reopen with full recovery verification, 1,000,000 point reads on eight threads, and compaction verification. All reported numbers are measured.

## Architecture

```text
            put(k, v)                              get(k)
               │                                     │
               ▼                                     ▼
   ┌──────────────────────┐          ┌──────────────────────────────┐
   │ 1. append to wal.log │          │ 1. memtable (std::map)       │
   │ 2. memtable[k] = v   │          │ 2. SSTables, newest → oldest │
   └──────────┬───────────┘          │    Bloom filter says "no"?   │
              │ memtable full        │      → skip table            │
              ▼                      │    else binary-search sparse │
   ┌──────────────────────┐          │      index, read one block   │
   │ flush → sst_N.sst    │          └──────────────────────────────┘
   │ truncate wal.log     │
   └──────────┬───────────┘
              │ compact()
              ▼
   ┌──────────────────────┐
   │ merge all SSTables → │
   │ one sst, newest wins │
   └──────────────────────┘
```

On disk, a database directory looks like this:

```text
data/
├── wal.log            # records not yet flushed
├── sst_000001.sst     # older
└── sst_000002.sst     # newer (a higher id means a newer table)
```

Both the WAL and SSTables use the same record encoding: `[u32 key_len][u32 value_len][key bytes][value bytes]`. An SSTable adds a header of `[u32 magic][u64 record_count]` in front of its records, which are sorted by key.

### Write path

1. `put` takes the exclusive lock.
2. The record is appended to `wal.log` and the stream is flushed to the operating system.
3. The memtable is updated.
4. If the memtable has reached its limit, it is flushed. The sorted entries are written to `sst_N.sst.tmp`, the file is renamed to `sst_N.sst`, the new table is opened, and `wal.log` is truncated.

Because the SSTable is renamed into place before the log is cleared, a crash at any point leaves each write in the log, in a complete table, or in both. On reopen, leftover `.tmp` files are deleted, SSTables are loaded in id order, and the WAL is replayed into the memtable. The WAL is then rewritten from the memtable, so a torn record at the end of the log (from a crash in the middle of an append) is dropped rather than left in front of new writes.

### Read path

1. `get` takes a shared lock, so many readers can run at the same time.
2. The memtable is checked first because it holds the newest data.
3. SSTables are then checked from newest to oldest, and the first match wins. For each table:
   - If the Bloom filter says the key is absent, the table is skipped and no disk read happens.
   - Otherwise, the sparse index is binary-searched to find the one 16-record block that could hold the key. That block is read from disk and scanned in sorted order.

The benchmark's skip rate is `skipped probes / (checked + skipped probes)` over the read phase.

### Compaction

`compact()` takes the exclusive lock and streams every SSTable from oldest to newest into a `std::map`, so a newer value for a key overwrites an older one. The merged result is written as a new table with the highest id, using the same temp-file-and-rename step as a flush. Only after that succeeds are the old tables closed and deleted. If the process crashes before the deletes, the reopened database has both the old tables and the merged table. Reads still return the correct values because the merged table is the newest.

## Build

Requires CMake 3.20 or newer and a C++20 compiler (GCC 10+ or Clang 12+).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Test

```bash
ctest --test-dir build --output-on-failure
# or, for per-test output:
./build/pebblekv_tests
```

To check for data races with ThreadSanitizer:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DPEBBLEKV_ENABLE_TSAN=ON
cmake --build build-tsan --target pebblekv_tests
./build-tsan/pebblekv_tests
```

## Benchmark

```bash
./build/pebblekv_benchmark
```

The benchmark runs these steps:

1. It deletes `./pebblekv_benchmark_data`.
2. It inserts 250,000 deterministic key-value pairs (13-byte keys, 64-byte values) in an order shuffled with a fixed seed.
3. It closes the database without a final flush, so the last 4,240 writes exist only in the WAL.
4. It reopens the database and verifies every stored key, plus one missing key between each pair of stored keys.
5. It performs exactly 1,000,000 point lookups across eight threads with fixed seeds. About half the lookups target keys that were never written, and every result is checked for correctness.
6. It compacts all tables into one and verifies a spread of stored and missing keys.

Timings use `std::chrono::steady_clock`.

### Example output (machine-dependent)

The run below was on a **single-core** Linux sandbox (GCC 13, Release build), so the eight reader threads were time-sliced, not truly parallel. Expect different numbers on your machine.

```text
writes=250000
write_ops_per_second=458964
reads=1000000
reader_threads=8
read_ops_per_second=493785
bloom_filter_skip_rate_percent=94.03
recovery_check=passed
compaction_check=passed
```

At the time of the read phase there are 15 SSTables. A stored key usually lets the filters skip the 14 tables that do not hold it. A missing key skips all 15 except for rare false positives. That mix produces a skip rate of about 94%. The skip rate is identical across runs because the seeds are fixed, while the throughput numbers vary slightly from run to run.

## Repository structure

```text
PebbleKV/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── .gitignore
├── include/
│   ├── bloom_filter.hpp     # Bloom filter interface
│   └── pebble_kv.hpp        # public PebbleKV API
├── src/
│   ├── bloom_filter.cpp     # bit vector + 3 hash probes
│   ├── pebble_kv.cpp        # WAL, memtable, SSTables, compaction, locking
│   └── benchmark.cpp        # benchmark executable
└── tests/
    └── test_pebble_kv.cpp   # standalone test executable
```

## Limitations

- **Not crash-durable against power loss.** The WAL is flushed to the operating system after each write but never `fsync`ed, because the C++ standard library has no `fsync`. The engine survives a process crash but not a kernel crash or power failure. Directory entries are not synced after renames either.
- **No deletes, range scans, or iterators.** The API is `put`, `get`, `flush`, and `compact` only, so there are no tombstones.
- **One global lock.** Writes, flushes, and compaction block all readers while they run. Compaction holds the exclusive lock for its whole duration.
- **Writers can starve.** On glibc, `std::shared_mutex` prefers readers, so a steady stream of `get` calls can hold off a `put` indefinitely.
- **Per-table read serialization.** Each SSTable shares one file stream behind a mutex, so concurrent reads of the same table take turns on the disk read. The Bloom filter check and in-memory index search happen outside that mutex.
- **Full-merge compaction.** Compaction loads every key and value into memory and rewrites everything. There are no levels, size tiers, or incremental compaction.
- **Bloom filters are rebuilt when a table is opened.** Each open scans the table once instead of loading a stored filter, so open time grows with data size.
- **No checksums.** Corruption is detected only through header and length checks. The file format uses native byte order, so files are not portable between big- and little-endian machines.
- **Single process only.** Nothing prevents two processes from opening the same directory at once.


## License

MIT. See [LICENSE](LICENSE).
