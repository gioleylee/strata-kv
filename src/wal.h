// Write-ahead log. Each record is: crc32(payload) | u32 length | payload, where
// payload = type byte | entry. Replay stops at the first short or corrupt
// record, which is how a torn write at the tail (power loss mid-append) is
// tolerated: everything before it was fully written and is recovered.
#pragma once

#include <functional>
#include <memory>

#include "util.h"

namespace strata {

class WalWriter {
 public:
  WalWriter(const std::string& path, bool sync, bool truncate)
      : file_(path, truncate ? "wb" : "ab"), sync_(sync) {}

  void Append(std::string_view key, std::string_view value, bool tombstone) {
    buf_.clear();
    encode_entry(buf_, key, value, tombstone);
    std::string rec;
    put_fixed32(rec, crc32(buf_.data(), buf_.size()));
    put_fixed32(rec, uint32_t(buf_.size()));
    rec.append(buf_);
    file_.Write(rec);
    // Hand every record to the OS before acknowledging the write. Left in the
    // stdio buffer, it would be lost if the process died, not just on power loss.
    if (sync_) file_.Sync();
    else file_.Flush();
  }

  void Sync() { file_.Sync(); }

 private:
  File file_;
  bool sync_;
  std::string buf_;
};

// Calls fn(key, value, tombstone) for each intact record; returns the count.
// `valid_bytes`, if given, receives the length of the intact prefix. The caller
// must cut the log back to that length before appending to it: a record written
// after a torn or corrupt one would never be reached by a later replay.
inline size_t ReplayWal(const std::string& path,
                        const std::function<void(std::string_view, std::string_view, bool)>& fn,
                        uint64_t* valid_bytes = nullptr) {
  if (valid_bytes) *valid_bytes = 0;
  FILE* probe = std::fopen(path.c_str(), "rb");
  if (!probe) return 0;
  std::fclose(probe);

  File f(path, "rb");
  size_t n = 0;
  std::string payload;
  char header[8];
  while (f.Read(header, 8) == 8) {
    uint32_t crc = get_fixed32(header), len = get_fixed32(header + 4);
    if (len > (1u << 30)) break;
    payload.resize(len);
    if (f.Read(payload.data(), len) != len) break;
    if (crc32(payload.data(), len) != crc) break;
    const char* p = payload.data();
    std::string_view k, v;
    bool tomb;
    if (!decode_entry(p, p + len, k, v, tomb)) break;
    fn(k, v, tomb);
    n++;
    if (valid_bytes) *valid_bytes += 8 + uint64_t(len);
  }
  return n;
}

}  // namespace strata
