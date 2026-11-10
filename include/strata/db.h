// Strata: a small LSM-tree key-value storage engine.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace strata {

struct Options {
  // Flush the memtable to an L0 SSTable once it holds roughly this many bytes.
  size_t memtable_bytes = 4 << 20;
  // Uncompressed size of an SSTable data block.
  size_t block_size = 4096;
  // Number of L0 files that triggers an L0 -> L1 compaction.
  int l0_compaction_trigger = 4;
  // Compaction output files are cut at roughly this size.
  size_t target_file_size = 8 << 20;
  // Bloom filter budget. 10 bits/key gives about a 1% false positive rate.
  int bloom_bits_per_key = 10;
  // fsync the write-ahead log after every write (durable, but slow).
  bool sync_wal = false;
};

struct Stats {
  uint64_t user_bytes_written = 0;   // key+value bytes passed to Put/Delete
  uint64_t bytes_flushed = 0;        // SSTable bytes written by memtable flushes
  uint64_t bytes_compacted = 0;      // SSTable bytes written by compactions
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  uint64_t bloom_checks = 0;         // SSTable lookups that consulted a filter
  uint64_t bloom_skips = 0;          // ...and were skipped because of it
  uint64_t block_reads = 0;          // data blocks read from disk by Get
  size_t l0_files = 0;
  size_t l1_files = 0;

  double write_amplification() const {
    return user_bytes_written
               ? double(bytes_flushed + bytes_compacted) / double(user_bytes_written)
               : 0.0;
  }
};

class DB {
 public:
  // Opens (creating if needed) a database in `dir`, replaying the write-ahead
  // log so that every acknowledged write from a previous run is visible.
  // Throws std::runtime_error on I/O failure or on-disk corruption.
  static std::unique_ptr<DB> Open(const std::string& dir, const Options& options = {});

  virtual ~DB() = default;

  virtual void Put(std::string_view key, std::string_view value) = 0;
  virtual void Delete(std::string_view key) = 0;
  virtual std::optional<std::string> Get(std::string_view key) = 0;

  // Visits live keys in [start, end) in ascending order; an empty `end` means
  // "no upper bound". Return false from `fn` to stop early.
  virtual void Scan(std::string_view start, std::string_view end,
                    const std::function<bool(std::string_view, std::string_view)>& fn) = 0;

  // Forces the memtable to disk / forces an L0 -> L1 compaction.
  virtual void Flush() = 0;
  virtual void Compact() = 0;

  virtual Stats GetStats() = 0;
};

}  // namespace strata
