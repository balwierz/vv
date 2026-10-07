// libFuzzer harness for R .rds / .RData files: vv's reader of R's
// serialization format (src/formats/rserial.cpp, after gzip / bzip2 / xz /
// zstd decoding) and the conversion of the object tree into tabs
// (src/formats/rdata.cpp), wide and --matrix long.
//
// The input is untrusted end to end: lengths, references, nesting, ALTREP
// states, S4 slots and sparse-matrix indices. The harness builds every tab's
// first chunks and checks each table with ValidateFull().
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_rdata
//   ./build-fuzz/fuzz_rdata -max_total_time=60 tests/data
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    rdata_fuzz_one(data, size);
    return 0;
}
