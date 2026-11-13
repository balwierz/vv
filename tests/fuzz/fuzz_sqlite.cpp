// libFuzzer harness for the SQLite reader (src/formats/sqlite.cpp), each
// opened from a file as vv opens it (source_fuzz_one, src/core/open.cpp). The
// first byte picks the extension; the rest is the file.
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_sqlite
//   ./build-fuzz/fuzz_sqlite -max_total_time=60 <corpus>
// A seed is a file with its extension's index byte in front: 0 sqlite.
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const char* const kExt[] = {"sqlite"};
    constexpr size_t kN = sizeof(kExt) / sizeof(kExt[0]);
    if (size < 1) return 0;
    source_fuzz_one(kExt[data[0] % kN], data + 1, size - 1);
    return 0;
}
