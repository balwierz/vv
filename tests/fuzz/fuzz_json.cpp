// libFuzzer harness for the JSON lexer behind `vv x.json` (pretty print),
// --json-paths and the stdin JSON sniff.
//
// The input is untrusted JSON. vvjson::fuzz_one runs the pretty printer, the
// paths writer and the sniff over it and checks two properties: an error
// offset lies within the input, and an accepted input pretty-prints to a
// fixed point (pretty(pretty(x)) == pretty(x)).
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_json
//   ./build-fuzz/fuzz_json -max_total_time=60
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    vvjson::fuzz_one(data, size);
    return 0;
}
