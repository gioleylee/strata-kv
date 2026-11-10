# Strata

[![ci](https://github.com/gioleylee/strata-kv/actions/workflows/ci.yml/badge.svg)](https://github.com/gioleylee/strata-kv/actions/workflows/ci.yml)

A small, readable **LSM-tree key-value storage engine** in C++20 with no dependencies
beyond the standard library. It implements the core of what sits underneath LevelDB and
RocksDB: a write-ahead log, a memtable, immutable sorted tables with bloom filters, and
compaction, with crash recovery that is actually tested.

```cpp
#include "strata/db.h"

auto db = strata::DB::Open("/tmp/mydb");
db->Put("user:42", "Ada");
db->Delete("user:7");
if (auto v = db->Get("user:42")) std::cout << *v << "\n";

db->Scan("user:", "user;", [](std::string_view k, std::string_view v) {
  std::cout << k << " = " << v << "\n";
  return true;                       // return false to stop early
});
```

## How it works

```
 Put/Delete ──► WAL (append, CRC per record) ──► memtable (sorted, in memory)
                                                    │ full
                                                    ▼
                               L0: SSTables, newest first, key ranges may overlap
                                                    │ 4 files
                                                    ▼  merge with overlapping L1 files
                               L1: SSTables, disjoint key ranges, sorted
```

**Write path.** A write is appended to the log and then applied to the memtable. When the
memtable reaches `memtable_bytes` it is written out as an SSTable in L0, the manifest is
replaced atomically (temp file + fsync + rename), and only then is the log truncated.

**Read path.** `Get` checks the memtable, then every L0 file from newest to oldest, then
binary-searches L1 for the single file whose range could hold the key. Each table consults
its bloom filter first, so a lookup for an absent key usually reads no data blocks at all.

**SSTable format.**

| Section | Contents |
| --- | --- |
| data blocks | entries in key order, about 4 KiB each, CRC32 per block |
| index block | smallest key, then `(last key, offset, size)` for each data block |
| bloom filter | 10 bits/key by default, double hashing |
| footer | offsets and sizes of the above, entry count, magic number |

**Compaction.** When L0 reaches four files they are merged, through a heap-based k-way
merging iterator, with only those L1 files whose key range overlaps. Newer versions shadow
older ones, and because L1 is the bottom level, tombstones are dropped. L1 files outside
the overlap are not rewritten, so sequential inserts cost one flush plus one move.

**Recovery.** On open, the manifest names the live tables; any other `.sst` file is a
leftover from an interrupted flush or compaction and is deleted. The log is replayed up to
the first record that is short or fails its checksum, which is how a torn final write is
discarded rather than half-applied.

## Tests

`strata_tests` is a dependency-free runner with eleven tests, including:

- **Model-based randomized test** – 60,000 random puts, deletes and gets are applied to the
  engine and to a `std::map`, with the database closed and reopened at random points (137
  times, across 57 compactions). Every read and a final full scan must agree with the model.
- **Crash recovery** – unflushed writes survive a reopen via the log; a log truncated
  mid-record loses only that record; a flipped byte stops replay at the damaged record.
- **Corruption detection** – a damaged data block raises an error instead of returning
  wrong data.
- **Bloom filter** – no false negatives; measured false positive rate 0.85% at 10 bits/key.

## Benchmark

1,000,000 operations, 16-byte keys, 100-byte values, default options, a single thread.
Measured on a Windows 11 desktop with an SSD, built with MinGW-w64 g++ 15.2 in Release mode.
These are numbers from one machine, meant to show the shape of the trade-offs rather than
to compare against other engines.

| Workload | Throughput | Notes |
| --- | ---: | --- |
| sequential fill | 242k ops/s | write amplification 2.09 |
| random fill | 127k ops/s | write amplification 4.24 |
| random read (key present) | 72k ops/s | one checksummed block read per lookup |
| random read (key absent) | 804k ops/s | 99.1% of table probes answered by the bloom filter |
| full scan | 2.18M keys/s | k-way merge over all levels |

Reproduce with `./build/strata_bench 1000000 100`.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Needs CMake 3.16+ and a C++20 compiler. CI builds and tests on Linux (GCC) and Windows (MSVC).

## Limitations

This is a learning-oriented engine, and the simplifications are deliberate:

- One mutex guards the whole database, and flushes and compactions run inline on the
  writing thread, so a write that triggers a compaction pays for it.
- There are two levels rather than a full leveled hierarchy, so write amplification grows
  with database size once L1 is large.
- No snapshots or MVCC, no write batches, no block cache or compression; data blocks are
  scanned linearly rather than using restart points.
- With `sync_wal = false` (the default) the log is not fsynced per write, so an OS crash or
  power loss can lose the most recent writes. A process crash cannot.

## Layout

```
include/strata/db.h   public API
src/util.h            varints, CRC32, bloom filter, file wrapper
src/wal.h             write-ahead log
src/sstable.h         table builder, reader and iterator
src/db.cc             memtable, merging iterator, flush, compaction, recovery
tests/                test runner
bench/                benchmark
```
