// Immutable sorted string tables.
//
//   [data block | crc32]*      entries in key order, ~block_size bytes each
//   [index block | crc32]      smallest key, then (last key, offset, size) per data block
//   [bloom filter]
//   [footer, 48 bytes]         index offset/size, bloom offset/size, entry count, magic
//
// The index and filter are held in memory; data blocks are read on demand, so a
// point lookup costs at most one block read (and usually zero for absent keys).
#pragma once

#include <algorithm>
#include <memory>
#include <mutex>

#include "util.h"

namespace strata {

constexpr uint64_t kTableMagic = 0x5354524154414442ull;  // "STRATADB"
constexpr size_t kFooterSize = 48;

class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view target) = 0;  // first entry with key >= target
  virtual void Next() = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  virtual bool tombstone() const = 0;
};

class TableBuilder {
 public:
  TableBuilder(const std::string& path, size_t block_size, int bits_per_key)
      : file_(path, "wb"), block_size_(block_size), bits_per_key_(bits_per_key) {}

  // Keys must be added in strictly ascending order.
  void Add(std::string_view key, std::string_view value, bool tombstone) {
    if (count_ == 0) smallest_.assign(key);
    encode_entry(block_, key, value, tombstone);
    last_key_.assign(key);
    hashes_.push_back(hash64(key));
    count_++;
    if (block_.size() >= block_size_) FlushBlock();
  }

  uint64_t EstimatedSize() const { return offset_ + block_.size(); }
  uint64_t count() const { return count_; }

  // Writes index, filter and footer, syncs the file; returns the final size.
  uint64_t Finish() {
    if (!block_.empty()) FlushBlock();
    std::string index_block;
    put_varint(index_block, smallest_.size());
    index_block.append(smallest_);
    index_block.append(index_);
    uint64_t index_off = offset_;
    WriteWithCrc(index_block);
    std::string bloom = Bloom::Build(hashes_, bits_per_key_);
    uint64_t bloom_off = offset_;
    file_.Write(bloom);
    offset_ += bloom.size();

    std::string footer;
    put_fixed64(footer, index_off);
    put_fixed64(footer, index_block.size());
    put_fixed64(footer, bloom_off);
    put_fixed64(footer, bloom.size());
    put_fixed64(footer, count_);
    put_fixed64(footer, kTableMagic);
    file_.Write(footer);
    offset_ += footer.size();
    file_.Sync();
    return offset_;
  }

 private:
  void FlushBlock() {
    put_varint(index_, last_key_.size());
    index_.append(last_key_);
    put_fixed64(index_, offset_);
    put_fixed32(index_, uint32_t(block_.size()));
    WriteWithCrc(block_);
    block_.clear();
  }
  void WriteWithCrc(const std::string& data) {
    std::string crc;
    put_fixed32(crc, crc32(data.data(), data.size()));
    file_.Write(data);
    file_.Write(crc);
    offset_ += data.size() + 4;
  }

  File file_;
  size_t block_size_;
  int bits_per_key_;
  std::string block_, index_, smallest_, last_key_;
  std::vector<uint64_t> hashes_;
  uint64_t offset_ = 0, count_ = 0;
};

class Table {
 public:
  Table(const std::string& path, uint64_t number) : path_(path), number_(number), file_(path, "rb") {
    uint64_t size = FileSize();
    if (size < kFooterSize) throw std::runtime_error("sstable too small: " + path);
    std::string footer = ReadRaw(size - kFooterSize, kFooterSize);
    if (get_fixed64(footer.data() + 40) != kTableMagic)
      throw std::runtime_error("bad sstable magic: " + path);
    uint64_t index_off = get_fixed64(footer.data()), index_size = get_fixed64(footer.data() + 8);
    uint64_t bloom_off = get_fixed64(footer.data() + 16), bloom_size = get_fixed64(footer.data() + 24);
    count_ = get_fixed64(footer.data() + 32);
    size_ = size;
    bloom_ = ReadRaw(bloom_off, bloom_size);

    std::string index = ReadChecked(index_off, index_size);
    const char* p = index.data();
    const char* end = p + index.size();
    uint64_t n;
    if (!get_varint(p, end, n) || uint64_t(end - p) < n) throw std::runtime_error("bad index: " + path);
    smallest_.assign(p, n);
    p += n;
    while (p < end) {
      if (!get_varint(p, end, n) || uint64_t(end - p) < n + 12)
        throw std::runtime_error("bad index: " + path);
      BlockHandle h;
      h.last_key.assign(p, n);
      p += n;
      h.offset = get_fixed64(p);
      h.size = get_fixed32(p + 8);
      p += 12;
      blocks_.push_back(std::move(h));
    }
    if (blocks_.empty()) throw std::runtime_error("empty sstable: " + path);
  }

  enum class Result { kNotFound, kFound, kDeleted, kFilteredOut };

  // Point lookup. kFilteredOut means the bloom filter proved the key absent
  // without touching a data block.
  Result Get(std::string_view key, std::string* value) {
    if (key < smallest_ || key > largest()) return Result::kNotFound;
    if (!Bloom::MayContain(bloom_, hash64(key))) return Result::kFilteredOut;
    size_t bi = FindBlock(key);
    if (bi == blocks_.size()) return Result::kNotFound;
    std::string block = ReadBlock(bi);
    const char* p = block.data();
    const char* end = p + block.size();
    std::string_view k, v;
    bool tomb;
    while (p < end && decode_entry(p, end, k, v, tomb)) {
      if (k == key) {
        if (tomb) return Result::kDeleted;
        value->assign(v);
        return Result::kFound;
      }
      if (k > key) break;
    }
    return Result::kNotFound;
  }

  std::unique_ptr<Iterator> NewIterator();

  const std::string& smallest() const { return smallest_; }
  const std::string& largest() const { return blocks_.back().last_key; }
  const std::string& path() const { return path_; }
  uint64_t number() const { return number_; }
  uint64_t size() const { return size_; }
  uint64_t count() const { return count_; }
  size_t num_blocks() const { return blocks_.size(); }

  // Index of the first block whose last key is >= key (blocks_.size() if none).
  size_t FindBlock(std::string_view key) const {
    auto it = std::lower_bound(blocks_.begin(), blocks_.end(), key,
                               [](const BlockHandle& h, std::string_view k) { return h.last_key < k; });
    return size_t(it - blocks_.begin());
  }

  std::string ReadBlock(size_t i) { return ReadChecked(blocks_[i].offset, blocks_[i].size); }

 private:
  struct BlockHandle {
    std::string last_key;
    uint64_t offset;
    uint32_t size;
  };

  uint64_t FileSize() {
    FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path_);
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    uint64_t n = uint64_t(_ftelli64(f));
#else
    fseeko(f, 0, SEEK_END);
    uint64_t n = uint64_t(ftello(f));
#endif
    std::fclose(f);
    return n;
  }

  std::string ReadRaw(uint64_t off, uint64_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string out(n, '\0');
    file_.Seek(off);
    if (file_.Read(out.data(), n) != n) throw std::runtime_error("short read: " + path_);
    return out;
  }

  std::string ReadChecked(uint64_t off, uint64_t n) {
    std::string raw = ReadRaw(off, n + 4);
    if (crc32(raw.data(), n) != get_fixed32(raw.data() + n))
      throw std::runtime_error("checksum mismatch in " + path_);
    raw.resize(n);
    return raw;
  }

  std::string path_;
  uint64_t number_;
  File file_;
  std::mutex mu_;
  std::string bloom_, smallest_;
  std::vector<BlockHandle> blocks_;
  uint64_t count_ = 0, size_ = 0;
};

class TableIterator : public Iterator {
 public:
  explicit TableIterator(Table* t) : t_(t) {}
  bool Valid() const override { return valid_; }
  void SeekToFirst() override {
    LoadBlock(0);
    Advance();
  }
  void Seek(std::string_view target) override {
    size_t bi = t_->FindBlock(target);
    if (bi == t_->num_blocks()) {
      valid_ = false;
      return;
    }
    LoadBlock(bi);
    Advance();
    while (valid_ && key_ < target) Advance();
  }
  void Next() override { Advance(); }
  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }
  bool tombstone() const override { return tomb_; }

 private:
  void LoadBlock(size_t i) {
    bi_ = i;
    block_ = t_->ReadBlock(i);
    p_ = block_.data();
    end_ = p_ + block_.size();
  }
  void Advance() {
    if (p_ == end_) {
      if (bi_ + 1 >= t_->num_blocks()) {
        valid_ = false;
        return;
      }
      LoadBlock(bi_ + 1);
    }
    valid_ = decode_entry(p_, end_, key_, value_, tomb_);
    if (!valid_) throw std::runtime_error("corrupt block in " + t_->path());
  }

  Table* t_;
  size_t bi_ = 0;
  std::string block_;
  const char* p_ = nullptr;
  const char* end_ = nullptr;
  std::string_view key_, value_;
  bool tomb_ = false, valid_ = false;
};

inline std::unique_ptr<Iterator> Table::NewIterator() { return std::make_unique<TableIterator>(this); }

}  // namespace strata
