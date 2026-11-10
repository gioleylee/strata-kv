// Dependency-free test runner. Each test gets a fresh database directory.
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <vector>

#include "../src/util.h"
#include "strata/db.h"

namespace fs = std::filesystem;
using namespace strata;

static int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("    CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

static std::string Key(int i) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "key%09d", i);
  return buf;
}

static Options SmallOptions() {
  Options o;
  o.memtable_bytes = 16 << 10;  // tiny, so tests exercise many flushes and compactions
  o.target_file_size = 32 << 10;
  o.block_size = 512;
  return o;
}

static void TestBasic(const std::string& dir) {
  auto db = DB::Open(dir);
  CHECK(!db->Get("a"));
  db->Put("a", "1");
  db->Put("b", "2");
  CHECK(db->Get("a") == "1");
  db->Put("a", "3");
  CHECK(db->Get("a") == "3");
  db->Delete("a");
  CHECK(!db->Get("a"));
  CHECK(db->Get("b") == "2");
  db->Put("", "empty key");
  db->Put("empty value", "");
  CHECK(db->Get("") == "empty key");
  CHECK(db->Get("empty value") == "");
}

static void TestDeleteShadowsOlderLevels(const std::string& dir) {
  auto db = DB::Open(dir);
  db->Put("k", "old");
  db->Compact();  // "old" now lives in L1
  db->Delete("k");
  CHECK(!db->Get("k"));
  db->Flush();    // tombstone in L0 must hide the L1 value
  CHECK(!db->Get("k"));
  db->Compact();  // tombstone and value are both gone
  CHECK(!db->Get("k"));
  db->Put("k", "new");
  CHECK(db->Get("k") == "new");
}

static void TestRecoveryFromWal(const std::string& dir) {
  {
    auto db = DB::Open(dir);
    for (int i = 0; i < 1000; i++) db->Put(Key(i), "v" + std::to_string(i));
    db->Delete(Key(7));
    // No Flush(): dropping the handle models a process crash, so the only
    // durable copy of these writes is the write-ahead log.
  }
  auto db = DB::Open(dir);
  CHECK(db->GetStats().l0_files == 0);
  for (int i = 0; i < 1000; i++) {
    if (i == 7) CHECK(!db->Get(Key(i)));
    else CHECK(db->Get(Key(i)) == "v" + std::to_string(i));
  }
}

static void TestTornWalTail(const std::string& dir) {
  {
    auto db = DB::Open(dir);
    for (int i = 0; i < 100; i++) db->Put(Key(i), "value");
  }
  // Chop bytes off the end, as if power failed in the middle of the last append.
  auto wal = dir + "/wal.log";
  fs::resize_file(wal, fs::file_size(wal) - 3);
  auto db = DB::Open(dir);
  for (int i = 0; i < 99; i++) CHECK(db->Get(Key(i)) == "value");
  CHECK(!db->Get(Key(99)));  // the torn record is discarded, not half-applied
  db->Put(Key(99), "again");
  CHECK(db->Get(Key(99)) == "again");
}

static void TestCorruptWalRecordStopsReplay(const std::string& dir) {
  {
    auto db = DB::Open(dir);
    for (int i = 0; i < 10; i++) db->Put(Key(i), "value");
  }
  auto wal = dir + "/wal.log";
  auto size = fs::file_size(wal);
  {
    std::FILE* f = std::fopen(wal.c_str(), "rb+");
    std::fseek(f, long(size / 2), SEEK_SET);
    std::fputc(0xFF, f);  // flip a byte in the middle of the log
    std::fclose(f);
  }
  auto db = DB::Open(dir);
  int found = 0;
  for (int i = 0; i < 10; i++) found += db->Get(Key(i)).has_value();
  CHECK(found >= 4 && found < 10);  // a prefix survives; the checksum catches the rest
}

static void TestPersistenceAcrossLevels(const std::string& dir) {
  const int n = 5000;
  {
    auto db = DB::Open(dir, SmallOptions());
    for (int i = 0; i < n; i++) db->Put(Key(i), std::string(40, char('a' + i % 26)));
    auto s = db->GetStats();
    CHECK(s.flushes > 4);
    CHECK(s.compactions > 0);
  }
  auto db = DB::Open(dir, SmallOptions());
  for (int i = 0; i < n; i++) CHECK(db->Get(Key(i)) == std::string(40, char('a' + i % 26)));
  CHECK(!db->Get(Key(n)));
}

static void TestScan(const std::string& dir) {
  auto db = DB::Open(dir, SmallOptions());
  for (int i = 0; i < 3000; i++) db->Put(Key(i), std::to_string(i));
  for (int i = 0; i < 3000; i += 3) db->Delete(Key(i));
  for (int i = 0; i < 3000; i += 5) db->Put(Key(i), "x");

  std::vector<std::string> seen;
  db->Scan(Key(100), Key(200), [&](std::string_view k, std::string_view v) {
    seen.emplace_back(k);
    int i = std::stoi(std::string(k.substr(3)));
    CHECK(v == (i % 5 == 0 ? "x" : std::to_string(i)));
    return true;
  });
  std::vector<std::string> expected;
  for (int i = 100; i < 200; i++)
    if (i % 3 != 0 || i % 5 == 0) expected.push_back(Key(i));
  CHECK(seen == expected);

  int visited = 0;
  db->Scan("", "", [&](std::string_view, std::string_view) { return ++visited < 10; });
  CHECK(visited == 10);  // early exit is honoured
}

// Model-based test: apply the same random operations to the engine and to a
// std::map, reopening the database at random points, and require that they
// never disagree on any point read or on a full scan.
static void TestRandomizedAgainstModel(const std::string& dir) {
  std::mt19937 rng(20261110);
  std::map<std::string, std::string> model;
  auto db = DB::Open(dir, SmallOptions());
  const int key_space = 2000;
  uint64_t compactions = 0, reopens = 0;  // stats are per handle, so sum across reopens
  for (int op = 0; op < 60000; op++) {
    std::string k = Key(int(rng() % key_space));
    int dice = int(rng() % 100);
    if (dice < 55) {
      std::string v(rng() % 120, char('a' + rng() % 26));
      db->Put(k, v);
      model[k] = v;
    } else if (dice < 75) {
      db->Delete(k);
      model.erase(k);
    } else if (dice < 99) {
      auto got = db->Get(k);
      auto it = model.find(k);
      CHECK(got.has_value() == (it != model.end()));
      if (got && it != model.end()) CHECK(*got == it->second);
    } else if (rng() % 4 == 0) {
      compactions += db->GetStats().compactions;
      reopens++;
      db.reset();  // "crash" and recover
      db = DB::Open(dir, SmallOptions());
    }
    if (g_failures) return;
  }
  auto it = model.begin();
  size_t n = 0;
  db->Scan("", "", [&](std::string_view k, std::string_view v) {
    CHECK(it != model.end() && it->first == k && it->second == v);
    if (it != model.end()) ++it;
    n++;
    return g_failures == 0;
  });
  CHECK(n == model.size());
  compactions += db->GetStats().compactions;
  std::printf("    %zu live keys, %llu compactions, %llu crash/reopen cycles\n", n,
              static_cast<unsigned long long>(compactions), static_cast<unsigned long long>(reopens));
  CHECK(compactions > 5 && reopens > 5);
}

static void TestSequentialInsertsSkipRewrites(const std::string& dir) {
  // Ascending keys never overlap existing L1 files, so compaction should only
  // rewrite the new data once: write amplification stays near 2 (flush + one move).
  auto db = DB::Open(dir, SmallOptions());
  for (int i = 0; i < 20000; i++) db->Put(Key(i), std::string(64, 'v'));
  db->Compact();
  CHECK(db->GetStats().write_amplification() < 3.0);
  CHECK(db->GetStats().l1_files > 10);
}

static void TestBloomFilter(const std::string&) {
  std::vector<uint64_t> hashes;
  for (int i = 0; i < 10000; i++) hashes.push_back(hash64(Key(i)));
  std::string filter = Bloom::Build(hashes, 10);
  for (int i = 0; i < 10000; i++) CHECK(Bloom::MayContain(filter, hash64(Key(i))));  // no false negatives
  int fp = 0;
  for (int i = 10000; i < 110000; i++) fp += Bloom::MayContain(filter, hash64(Key(i)));
  double rate = fp / 100000.0;
  std::printf("    bloom false positive rate at 10 bits/key: %.2f%%\n", rate * 100);
  CHECK(rate < 0.02);
}

static void TestCorruptTableIsDetected(const std::string& dir) {
  {
    auto db = DB::Open(dir);
    for (int i = 0; i < 2000; i++) db->Put(Key(i), std::string(50, 'z'));
    db->Flush();
  }
  for (auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() != ".sst") continue;
    std::FILE* f = std::fopen(e.path().string().c_str(), "rb+");
    std::fseek(f, 100, SEEK_SET);
    std::fputc('!', f);  // damage the first data block
    std::fclose(f);
  }
  auto db = DB::Open(dir);
  bool threw = false;
  try {
    db->Get(Key(0));
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);  // corruption surfaces as an error, never as wrong data
}

int main() {
  struct Test {
    const char* name;
    void (*fn)(const std::string&);
  };
  const Test tests[] = {
      {"basic put/get/delete", TestBasic},
      {"delete shadows older levels", TestDeleteShadowsOlderLevels},
      {"recovery from WAL", TestRecoveryFromWal},
      {"torn WAL tail", TestTornWalTail},
      {"corrupt WAL record stops replay", TestCorruptWalRecordStopsReplay},
      {"persistence across levels", TestPersistenceAcrossLevels},
      {"range scan", TestScan},
      {"randomized ops vs. std::map model", TestRandomizedAgainstModel},
      {"sequential inserts skip rewrites", TestSequentialInsertsSkipRewrites},
      {"bloom filter", TestBloomFilter},
      {"corrupt SSTable is detected", TestCorruptTableIsDetected},
  };
  fs::path root = fs::temp_directory_path() / "strata_tests";
  int failed = 0, i = 0;
  for (auto& t : tests) {
    fs::path dir = root / std::to_string(i++);
    fs::remove_all(dir);
    int before = g_failures;
    try {
      t.fn(dir.string());
    } catch (const std::exception& e) {
      std::printf("    unexpected exception: %s\n", e.what());
      g_failures++;
    }
    bool ok = g_failures == before;
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", t.name);
    failed += !ok;
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  std::printf("%d/%d tests passed\n", i - failed, i);
  return failed ? 1 : 0;
}
