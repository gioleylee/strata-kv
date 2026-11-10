// Micro-benchmark: sequential fill, random fill, random point reads (hits and
// misses) and a full scan. Usage: strata_bench [num_keys] [value_size]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

#include "strata/db.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static std::string Key(uint64_t i) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llu", static_cast<unsigned long long>(i));
  return buf;
}

template <typename F>
static void Run(const char* name, uint64_t ops, F body) {
  auto t0 = Clock::now();
  body();
  double s = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("%-14s %10.0f ops/s  %8.2f us/op\n", name, ops / s, s * 1e6 / ops);
}

int main(int argc, char** argv) {
  uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 500000;
  size_t value_size = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 100;
  fs::path dir = fs::temp_directory_path() / "strata_bench";
  std::string value(value_size, 'x');
  std::printf("keys: %llu, key size: 16, value size: %zu\n", static_cast<unsigned long long>(n), value_size);

  for (bool sequential : {true, false}) {
    fs::remove_all(dir);
    auto db = strata::DB::Open(dir.string());
    std::mt19937_64 rng(42);
    Run(sequential ? "fillseq" : "fillrandom", n, [&] {
      for (uint64_t i = 0; i < n; i++) db->Put(Key(sequential ? i : rng() % n), value);
      db->Flush();
    });
    auto s = db->GetStats();
    std::printf("  write amplification %.2f, %zu L0 + %zu L1 files, %llu compactions\n",
                s.write_amplification(), s.l0_files, s.l1_files,
                static_cast<unsigned long long>(s.compactions));
    if (sequential) continue;

    uint64_t reads = n / 2, found = 0;
    Run("readrandom", reads, [&] {
      for (uint64_t i = 0; i < reads; i++) found += db->Get(Key(rng() % n)).has_value();
    });
    auto before = db->GetStats();
    Run("readmissing", reads, [&] {
      for (uint64_t i = 0; i < reads; i++) found += db->Get(Key(rng() % n) + "x").has_value();
    });
    auto after = db->GetStats();
    std::printf("  misses resolved by bloom filter: %.1f%%\n",
                100.0 * double(after.bloom_skips - before.bloom_skips) /
                    double(std::max<uint64_t>(1, after.bloom_checks - before.bloom_checks)));
    uint64_t scanned = 0;
    Run("scan", n, [&] {
      db->Scan("", "", [&](std::string_view, std::string_view) { return ++scanned > 0; });
    });
    std::printf("  (%llu live keys scanned, %llu point reads hit)\n",
                static_cast<unsigned long long>(scanned), static_cast<unsigned long long>(found));
  }
  fs::remove_all(dir);
}
