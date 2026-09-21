// libFuzzer harness for the LZF decoder behind HDF5 filter 32000.
//
// LZF-compressed .h5ad / .h5 chunks (h5py's compression="lzf") are decoded by
// h5lzf::decode, which reads offsets and lengths straight from the file. Fuzz it
// directly: the first input byte picks the output capacity (so both the
// "fits" and the "output too small" paths run), the rest is the stream.
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_lzf
//   ./build-fuzz/fuzz_lzf -max_total_time=60
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    size_t out_len = (size_t)data[0] * 64;          // 0 .. 16320 bytes
    std::vector<uint8_t> out(out_len);
    bool too_small = false;
    size_t n = h5lzf::decode(data + 1, size - 1, out.data(), out_len, &too_small);
    if (n > out_len) std::abort();                  // never report past the buffer
    if (n > 0 && too_small) std::abort();           // success and overflow are exclusive
    return 0;
}
