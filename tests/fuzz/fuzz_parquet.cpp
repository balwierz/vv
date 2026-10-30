// libFuzzer harness for the Parquet reader (src/formats/parquet.cpp): the
// file opened as vv opens it, its first rows (read_first's row-group
// selection) and every row group read and checked with ValidateFull().
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_parquet
//   ./build-fuzz/fuzz_parquet -max_total_time=60 <corpus of .parquet files>
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    source_fuzz_one("parquet", data, size);
    return 0;
}
