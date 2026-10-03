// libFuzzer harness for R .rds / .RData files (the vendored librdata parser and
// vv's conversion of its callbacks into Arrow tables).
//
// librdata decodes R's serialization format (and gzip / bzip2 / xz) from
// untrusted bytes; the conversion trusts the counts it reports. The harness
// runs both over an in-memory buffer and checks every built table with
// ValidateFull().
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
