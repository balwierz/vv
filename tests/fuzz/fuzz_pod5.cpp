// libFuzzer harness for Oxford Nanopore POD5 files: the FlatBuffers footer
// reader, the embedded Arrow IPC tables (read through a slice of the file),
// the UUID conversion and the VBZ signal decoder (zstd + svb16 zigzag deltas)
// — hand-rolled parsing of untrusted bytes. Every built table is checked with
// ValidateFull().
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_pod5
//   ./build-fuzz/fuzz_pod5 -max_total_time=60 <corpus with tests/data/tiny.pod5>
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    pod5_fuzz_one(data, size);
    return 0;
}
