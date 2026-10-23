// libFuzzer harness for the multiple-alignment reader (src/formats/msa.cpp):
// every format's parser (Stockholm, Clustal, PHYLIP, NEXUS, MSF, A2M / A3M,
// aligned FASTA) on the same untrusted text, and the tables built from what
// each accepts, checked with ValidateFull().
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_msa
//   ./build-fuzz/fuzz_msa -max_total_time=60 tests/data
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    msa_fuzz_one(data, size);
    return 0;
}
