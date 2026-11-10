#include "strata/db.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <set>

#include "sstable.h"
#include "wal.h"

namespace fs = std::filesystem;

namespace strata {
namespace {

struct MemValue {
  std::string value;
  bool tombstone;
};
using MemTable = std::map<std::string, MemValue, std::less<>>;

class MemIterator : public Iterator {
 public:
  explicit MemIterator(const MemTable& m) : m_(m), it_(m.end()) {}
  bool Valid() const override { return it_ != m_.end(); }
  void SeekToFirst() override { it_ = m_.begin(); }
  void Seek(std::string_view t) override { it_ = m_.lower_bound(t); }
  void Next() override { ++it_; }
  std::string_view key() const override { return it_->first; }
  std::string_view value() const override { return it_->second.value; }
  bool tombstone() const override { return it_->second.tombstone; }

 private:
  const MemTable& m_;
  MemTable::const_iterator it_;
};

// K-way merge over child iterators. Children are ordered newest-first; when
// several hold the same key only the newest version is surfaced, which is what
// gives overwrites and deletes their "last write wins" meaning across levels.
class MergingIterator : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)), heap_(Cmp{this}) {}

  bool Valid() const override { return !heap_.empty(); }
  void SeekToFirst() override {
    Reset([](Iterator& it) { it.SeekToFirst(); });
  }
  void Seek(std::string_view target) override {
    Reset([&](Iterator& it) { it.Seek(target); });
  }
  void Next() override {
    std::string current(key());
    // Advance every child positioned at the current key (older versions are shadowed).
    while (!heap_.empty() && children_[heap_.top()]->key() == current) {
      size_t i = heap_.top();
      heap_.pop();
      children_[i]->Next();
      if (children_[i]->Valid()) heap_.push(i);
    }
  }
  std::string_view key() const override { return children_[heap_.top()]->key(); }
  std::string_view value() const override { return children_[heap_.top()]->value(); }
  bool tombstone() const override { return children_[heap_.top()]->tombstone(); }

 private:
  struct Cmp {
    const MergingIterator* self;
    // priority_queue is a max-heap, so "less" means "comes later".
    bool operator()(size_t a, size_t b) const {
      int c = self->children_[a]->key().compare(self->children_[b]->key());
      return c != 0 ? c > 0 : a > b;
    }
  };
  template <typename F>
  void Reset(F position) {
    heap_ = std::priority_queue<size_t, std::vector<size_t>, Cmp>(Cmp{this});
    for (size_t i = 0; i < children_.size(); i++) {
      position(*children_[i]);
      if (children_[i]->Valid()) heap_.push(i);
    }
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  std::priority_queue<size_t, std::vector<size_t>, Cmp> heap_;
};

using TablePtr = std::shared_ptr<Table>;

class DBImpl : public DB {
 public:
  DBImpl(const std::string& dir, const Options& opt) : dir_(dir), opt_(opt) {
    fs::create_directories(dir_);
    LoadManifest();
    RemoveOrphans();
    uint64_t valid_bytes = 0;
    ReplayWal(WalPath(), [&](std::string_view k, std::string_view v, bool tomb) {
      Apply(k, v, tomb);
    }, &valid_bytes);
    // Drop a torn or corrupt tail so new records are appended directly after
    // the last intact one. Otherwise the next replay would stop at the damaged
    // record and silently lose every write acknowledged after this point.
    std::error_code ec;
    if (auto size = fs::file_size(WalPath(), ec); !ec && size > valid_bytes) {
      fs::resize_file(WalPath(), valid_bytes);
    }
    wal_ = std::make_unique<WalWriter>(WalPath(), opt_.sync_wal, /*truncate=*/false);
    if (mem_bytes_ >= opt_.memtable_bytes) FlushLocked();
  }

  void Put(std::string_view key, std::string_view value) override { Write(key, value, false); }
  void Delete(std::string_view key) override { Write(key, {}, true); }

  std::optional<std::string> Get(std::string_view key) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (auto it = mem_.find(key); it != mem_.end()) {
      if (it->second.tombstone) return std::nullopt;
      return it->second.value;
    }
    std::string value;
    // L0 files may overlap, so check each, newest first.
    for (auto& t : l0_) {
      auto r = Lookup(*t, key, &value);
      if (r == Table::Result::kFound) return value;
      if (r == Table::Result::kDeleted) return std::nullopt;
    }
    // L1 files are disjoint and sorted: at most one can contain the key.
    auto it = std::lower_bound(l1_.begin(), l1_.end(), key,
                               [](const TablePtr& t, std::string_view k) { return t->largest() < k; });
    if (it != l1_.end() && Lookup(**it, key, &value) == Table::Result::kFound) return value;
    return std::nullopt;
  }

  void Scan(std::string_view start, std::string_view end,
            const std::function<bool(std::string_view, std::string_view)>& fn) override {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::unique_ptr<Iterator>> children;
    children.push_back(std::make_unique<MemIterator>(mem_));
    for (auto& t : l0_) children.push_back(t->NewIterator());
    for (auto& t : l1_)
      if (end.empty() || t->smallest() < end) children.push_back(t->NewIterator());
    MergingIterator it(std::move(children));
    for (it.Seek(start); it.Valid(); it.Next()) {
      if (!end.empty() && it.key() >= end) break;
      if (it.tombstone()) continue;
      if (!fn(it.key(), it.value())) break;
    }
  }

  void Flush() override {
    std::lock_guard<std::mutex> lock(mu_);
    FlushLocked();
  }

  void Compact() override {
    std::lock_guard<std::mutex> lock(mu_);
    FlushLocked();
    CompactLocked();
  }

  Stats GetStats() override {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.l0_files = l0_.size();
    stats_.l1_files = l1_.size();
    return stats_;
  }

 private:
  std::string WalPath() const { return dir_ + "/wal.log"; }
  std::string ManifestPath() const { return dir_ + "/MANIFEST"; }
  std::string TablePath(uint64_t n) const {
    char buf[32];
    std::snprintf(buf, sizeof buf, "/%06llu.sst", static_cast<unsigned long long>(n));
    return dir_ + buf;
  }

  void Apply(std::string_view k, std::string_view v, bool tomb) {
    auto it = mem_.find(k);
    if (it == mem_.end()) {
      mem_bytes_ += k.size() + v.size() + 32;
      mem_.emplace(std::string(k), MemValue{std::string(v), tomb});
    } else {
      mem_bytes_ += v.size();
      mem_bytes_ -= std::min(mem_bytes_, it->second.value.size());
      it->second = MemValue{std::string(v), tomb};
    }
  }

  void Write(std::string_view key, std::string_view value, bool tomb) {
    std::lock_guard<std::mutex> lock(mu_);
    // Log first, then apply: a write is acknowledged only once it is in the WAL.
    wal_->Append(key, value, tomb);
    Apply(key, value, tomb);
    stats_.user_bytes_written += key.size() + value.size();
    if (mem_bytes_ >= opt_.memtable_bytes) FlushLocked();
  }

  Table::Result Lookup(Table& t, std::string_view key, std::string* value) {
    stats_.bloom_checks++;
    auto r = t.Get(key, value);
    if (r == Table::Result::kFilteredOut) {
      stats_.bloom_skips++;
    } else if (key >= t.smallest() && key <= t.largest()) {
      stats_.block_reads++;
    }
    return r;
  }

  // The manifest is the source of truth for which SSTables are live. It is
  // replaced atomically (write temp, fsync, rename), so a crash leaves either
  // the old or the new version, never a mix.
  void LoadManifest() {
    std::ifstream in(ManifestPath());
    if (!in) return;
    std::string tag;
    uint64_t n;
    while (in >> tag >> n) {
      if (tag == "next") {
        next_file_ = n;
      } else if (tag == "L0") {
        l0_.push_back(std::make_shared<Table>(TablePath(n), n));
      } else if (tag == "L1") {
        l1_.push_back(std::make_shared<Table>(TablePath(n), n));
      } else {
        throw std::runtime_error("corrupt manifest in " + dir_);
      }
    }
  }

  void SaveManifest() {
    std::string body = "next " + std::to_string(next_file_) + "\n";
    for (auto& t : l0_) body += "L0 " + std::to_string(t->number()) + "\n";
    for (auto& t : l1_) body += "L1 " + std::to_string(t->number()) + "\n";
    std::string tmp = ManifestPath() + ".tmp";
    {
      File f(tmp, "wb");
      f.Write(body);
      f.Sync();
    }
    fs::rename(tmp, ManifestPath());
  }

  // Deletes SSTables not referenced by the manifest: leftovers from a flush or
  // compaction that crashed before it could commit.
  void RemoveOrphans() {
    std::set<std::string> live;
    for (auto& t : l0_) live.insert(fs::path(t->path()).filename().string());
    for (auto& t : l1_) live.insert(fs::path(t->path()).filename().string());
    for (auto& e : fs::directory_iterator(dir_)) {
      auto name = e.path().filename().string();
      bool stale_table = e.path().extension() == ".sst" && !live.count(name);
      if (stale_table || name == "MANIFEST.tmp") fs::remove(e.path());
    }
  }

  void FlushLocked() {
    if (mem_.empty()) return;
    uint64_t n = next_file_++;
    {
      TableBuilder b(TablePath(n), opt_.block_size, opt_.bloom_bits_per_key);
      for (auto& [k, v] : mem_) b.Add(k, v.value, v.tombstone);
      stats_.bytes_flushed += b.Finish();
    }  // closes the write handle before the table is reopened for reading
    stats_.flushes++;

    l0_.insert(l0_.begin(), std::make_shared<Table>(TablePath(n), n));
    SaveManifest();
    // Only now is it safe to drop the log: its contents are durable in the
    // SSTable. A crash before this line just replays the log into a memtable
    // whose entries duplicate (and are shadowed identically to) the new table.
    wal_.reset();
    wal_ = std::make_unique<WalWriter>(WalPath(), opt_.sync_wal, /*truncate=*/true);
    mem_.clear();
    mem_bytes_ = 0;
    if (int(l0_.size()) >= opt_.l0_compaction_trigger) CompactLocked();
  }

  // Merges all of L0 with the L1 files whose key range overlaps it. L1 files
  // outside that range are left untouched, which is what makes sequential
  // inserts cheap. L1 is the bottom level, so tombstones are dropped here.
  void CompactLocked() {
    if (l0_.empty()) return;
    std::string lo = l0_[0]->smallest(), hi = l0_[0]->largest();
    for (auto& t : l0_) {
      lo = std::min(lo, t->smallest());
      hi = std::max(hi, t->largest());
    }
    std::vector<TablePtr> inputs = l0_, keep;
    for (auto& t : l1_) {
      if (t->largest() < lo || t->smallest() > hi) keep.push_back(t);
      else inputs.push_back(t);
    }

    std::vector<std::unique_ptr<Iterator>> children;
    for (auto& t : inputs) children.push_back(t->NewIterator());
    MergingIterator it(std::move(children));

    std::vector<TablePtr> outputs;
    std::unique_ptr<TableBuilder> builder;
    uint64_t number = 0;
    auto finish = [&] {
      if (!builder) return;
      stats_.bytes_compacted += builder->Finish();
      builder.reset();
      outputs.push_back(std::make_shared<Table>(TablePath(number), number));
    };
    for (it.SeekToFirst(); it.Valid(); it.Next()) {
      if (it.tombstone()) continue;
      if (!builder) {
        number = next_file_++;
        builder = std::make_unique<TableBuilder>(TablePath(number), opt_.block_size,
                                                 opt_.bloom_bits_per_key);
      }
      builder->Add(it.key(), it.value(), false);
      if (builder->EstimatedSize() >= opt_.target_file_size) finish();
    }
    finish();

    keep.insert(keep.end(), outputs.begin(), outputs.end());
    std::sort(keep.begin(), keep.end(),
              [](const TablePtr& a, const TablePtr& b) { return a->smallest() < b->smallest(); });
    l0_.clear();
    l1_ = std::move(keep);
    SaveManifest();
    stats_.compactions++;

    // Inputs are obsolete once the manifest no longer names them. Close each
    // file before unlinking (required on Windows).
    std::vector<std::string> dead;
    for (auto& t : inputs) dead.push_back(t->path());
    outputs.clear();
    inputs.clear();
    for (auto& p : dead) fs::remove(p);
  }

  std::string dir_;
  Options opt_;
  std::mutex mu_;
  MemTable mem_;
  size_t mem_bytes_ = 0;
  std::unique_ptr<WalWriter> wal_;
  std::vector<TablePtr> l0_;  // newest first; key ranges may overlap
  std::vector<TablePtr> l1_;  // sorted by key; ranges are disjoint
  uint64_t next_file_ = 1;
  Stats stats_;
};

}  // namespace

std::unique_ptr<DB> DB::Open(const std::string& dir, const Options& options) {
  return std::make_unique<DBImpl>(dir, options);
}

}  // namespace strata
