// libFuzzer harness for the HDF5 readers (src/formats/hdf5.cpp: AnnData,
// Loom, 10x, generic HDF5; src/formats/cooler.cpp: .cool / .mcool / .scool),
// each opened from a file as vv opens it (source_fuzz_one,
// src/core/open.cpp). The first byte picks the extension; the rest is the
// file.
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_hdf5
//   ./build-fuzz/fuzz_hdf5 -max_total_time=60 <corpus>
// A seed is a file with its extension's index byte in front: 0 h5ad, 1 loom,
// 2 h5, 3 hdf5, 4 cool, 5 mcool, 6 scool.
//
// Not in CI's fuzz smoke run: libhdf5 itself loops on some inputs (H5Aread
// of a variable-length string attribute, in its global-heap code), so a run
// stops at its first timeout. Run it locally with -timeout=4 and restart
// past timeouts; see TODO.md, "HDF5 / Cooler fuzzing stops in libhdf5".
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const char* const kExt[] = {"h5ad", "loom", "h5", "hdf5", "cool", "mcool", "scool"};
    constexpr size_t kN = sizeof(kExt) / sizeof(kExt[0]);
    if (size < 1) return 0;
    source_fuzz_one(kExt[data[0] % kN], data + 1, size - 1);
    return 0;
}
