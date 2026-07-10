// libFuzzer harness for the NumPy .npy parser + array builders.
//
// The .npy header (magic, version, the Python-dict header with shape / dtype /
// fortran_order) and the array-body readers are hand-rolled and parse untrusted
// bytes (a .npz is a zip of these). Past audit findings here: negative / product-
// overflowing shapes, and a declared shape that doesn't fit the body. Fuzz the
// whole parse->build path (parse_npy_header validates the shape against the
// buffer, so an accepted header can't drive the builders out of bounds).
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_npy
//   ./build-fuzz/fuzz_npy -max_total_time=60
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    npz::npy_fuzz_one(data, size);
    return 0;
}
