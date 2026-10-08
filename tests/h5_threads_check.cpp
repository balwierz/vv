// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// HDF5-backed sources read from several threads at once, as vvg does when an
// export or the Column tab scans a second copy of a tab while the table reads
// the first. libhdf5 is usually built without its thread-safe option, so vv
// serialises every call into it; without that this check fails with HDF5
// error stacks, wrong row counts or a crash.
//
//   h5_threads_check FILE...      exit 0 when every read matched

#include "vv/vvcore.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

// Rows read across every tab of `path`; -1 on an open or read error.
int64_t read_all(const std::string& path) {
    Config cfg;
    cfg.describe = true;   // an AnnData frame is read in full, as for statistics
    std::unique_ptr<TabularSource> src;
    if (!open_source(path, cfg, &src).empty() || !src) return -1;
    std::vector<std::unique_ptr<TabularSource>> tabs;
    for (auto& s : src->expand_tabs()) tabs.push_back(std::move(s));
    tabs.insert(tabs.begin(), std::move(src));
    int64_t rows = 0;
    for (auto& t : tabs) {
        std::vector<int> cols;
        for (int i = 0; i < t->schema()->num_fields(); ++i) cols.push_back(i);
        for (int c = 0; ; ++c) {
            t->ensure(c);
            if (c >= t->num_chunks()) break;
            std::shared_ptr<arrow::Table> tbl;
            if (!t->read_chunk(c, cols, &tbl).ok() || !tbl) return -1;
            rows += tbl->num_rows();
        }
        if (!t->read_status().ok()) return -1;
    }
    return rows;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s FILE...\n", argv[0]); return 2; }
    std::vector<std::string> files(argv + 1, argv + argc);
    std::vector<int64_t> want;
    for (const auto& f : files) {
        want.push_back(read_all(f));
        if (want.back() < 0) { std::fprintf(stderr, "%s: cannot read\n", f.c_str()); return 2; }
    }
    constexpr int kThreads = 4, kRounds = 60;
    std::atomic<int> bad{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t)
        pool.emplace_back([&, t] {
            for (int r = 0; r < kRounds; ++r) {
                const size_t i = (size_t)(t + r) % files.size();
                if (read_all(files[i]) != want[i]) ++bad;
            }
        });
    for (auto& th : pool) th.join();
    if (bad) { std::fprintf(stderr, "%d of %d concurrent reads differed\n", bad.load(), kThreads * kRounds); return 1; }
    std::printf("ok: %d concurrent reads of %zu files\n", kThreads * kRounds, files.size());
    return 0;
}
