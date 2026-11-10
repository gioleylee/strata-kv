// Encoding helpers, checksums, hashing and a thin RAII wrapper over FILE*.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace strata {

inline uint32_t crc32(const char* data, size_t n) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = table[(c ^ uint8_t(data[i])) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// Fixed-width integers are stored in host byte order (little-endian on every
// platform this is built for).
inline void put_fixed32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
inline void put_fixed64(std::string& s, uint64_t v) { s.append(reinterpret_cast<const char*>(&v), 8); }
inline uint32_t get_fixed32(const char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t get_fixed64(const char* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

inline void put_varint(std::string& s, uint64_t v) {
  while (v >= 0x80) {
    s.push_back(char(v | 0x80));
    v >>= 7;
  }
  s.push_back(char(v));
}

inline bool get_varint(const char*& p, const char* end, uint64_t& v) {
  v = 0;
  for (int shift = 0; shift <= 63 && p < end; shift += 7) {
    uint8_t b = uint8_t(*p++);
    v |= uint64_t(b & 0x7F) << shift;
    if (!(b & 0x80)) return true;
  }
  return false;
}

// Entry layout shared by data blocks: varint klen | varint (vlen<<1 | tombstone) | key | value
inline void encode_entry(std::string& out, std::string_view k, std::string_view v, bool tombstone) {
  put_varint(out, k.size());
  put_varint(out, (uint64_t(v.size()) << 1) | (tombstone ? 1 : 0));
  out.append(k);
  out.append(v);
}

inline bool decode_entry(const char*& p, const char* end, std::string_view& k,
                         std::string_view& v, bool& tombstone) {
  uint64_t kl, vl;
  if (!get_varint(p, end, kl) || !get_varint(p, end, vl)) return false;
  tombstone = vl & 1;
  vl >>= 1;
  if (uint64_t(end - p) < kl + vl) return false;
  k = std::string_view(p, kl);
  p += kl;
  v = std::string_view(p, vl);
  p += vl;
  return true;
}

// FNV-1a followed by a murmur-style finalizer for better bit diffusion.
inline uint64_t hash64(std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  return h;
}

// Bloom filter using double hashing (Kirsch & Mitzenmacher): the i-th probe is
// h1 + i*h2, so one 64-bit hash per key is enough.
class Bloom {
 public:
  static std::string Build(const std::vector<uint64_t>& hashes, int bits_per_key) {
    int k = std::max(1, std::min(30, int(bits_per_key * 0.69)));
    size_t bits = std::max<size_t>(64, hashes.size() * size_t(bits_per_key));
    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;
    std::string out(bytes, '\0');
    for (uint64_t h : hashes) {
      uint64_t delta = (h >> 32) | 1;
      for (int i = 0; i < k; i++) {
        size_t bit = h % bits;
        out[bit / 8] |= char(1 << (bit % 8));
        h += delta;
      }
    }
    out.push_back(char(k));
    return out;
  }

  static bool MayContain(std::string_view filter, uint64_t h) {
    if (filter.size() < 2) return true;
    size_t bits = (filter.size() - 1) * 8;
    int k = filter.back();
    uint64_t delta = (h >> 32) | 1;
    for (int i = 0; i < k; i++) {
      size_t bit = h % bits;
      if (!(filter[bit / 8] & (1 << (bit % 8)))) return false;
      h += delta;
    }
    return true;
  }
};

class File {
 public:
  File(const std::string& path, const char* mode) : f_(std::fopen(path.c_str(), mode)), path_(path) {
    if (!f_) throw std::runtime_error("cannot open " + path);
  }
  ~File() { if (f_) std::fclose(f_); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  void Write(std::string_view s) {
    if (std::fwrite(s.data(), 1, s.size(), f_) != s.size())
      throw std::runtime_error("write failed: " + path_);
  }
  // Reads up to n bytes; returns the number actually read.
  size_t Read(char* buf, size_t n) { return std::fread(buf, 1, n, f_); }
  void Seek(uint64_t off) {
#ifdef _WIN32
    int rc = _fseeki64(f_, int64_t(off), SEEK_SET);
#else
    int rc = fseeko(f_, off_t(off), SEEK_SET);
#endif
    if (rc != 0) throw std::runtime_error("seek failed: " + path_);
  }
  void Flush() { std::fflush(f_); }
  // Flushes user-space buffers and asks the OS to persist to stable storage.
  void Sync() {
    std::fflush(f_);
#ifdef _WIN32
    _commit(_fileno(f_));
#else
    fsync(fileno(f_));
#endif
  }

 private:
  FILE* f_;
  std::string path_;
};

}  // namespace strata
