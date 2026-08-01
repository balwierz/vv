// libFuzzer harness for the LociSSD v4 "colblock" codec decoder.
//
// decode_colblock() is where the DICT / ARENA string decoders read
// attacker-controlled offsets out of a decompressed chunk (the class of bug
// hardened in #71). Fuzzing it directly — a pure function, no file I/O per
// iteration — gives high coverage of every codec path (RAW / DELTA / LENGTH /
// DICT / FRONTCODE / ARENA / BOOL) and the null-bitmap prefix. Build + run:
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_colblock
//   ./build-fuzz/fuzz_colblock -max_total_time=60
//
// A returned Status::Invalid is the expected outcome for malformed input; the
// harness only fails if the sanitizers (ASan/UBSan, compiled in) trip on an
// out-of-bounds read, use-after-free, integer overflow, etc.
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <arrow/type.h>

static std::shared_ptr<arrow::DataType> pick_type(uint8_t sel) {
    switch (sel % 8) {
        case 0:  return arrow::int8();
        case 1:  return arrow::int16();
        case 2:  return arrow::int32();
        case 3:  return arrow::int64();
        case 4:  return arrow::float32();
        case 5:  return arrow::float64();
        case 6:  return arrow::utf8();
        default: return arrow::large_utf8();
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 5) return 0;
    // First 5 bytes select codec / type / coord-width / row count; the rest is
    // the (decompressed) chunk payload the decoder must validate against `n`.
    int     codec = data[0] % 7;                       // 0=RAW .. 6=BOOL
    auto    type  = pick_type(data[1]);
    int     cw    = (data[2] & 1) ? 8 : 4;             // int64 vs int32 coords
    int64_t n     = (int64_t)(((unsigned)data[3] << 8) | data[4]) % 2048;

    const uint8_t* buf  = data + 5;
    size_t         blen = size - 5;

    // LENGTH (codec 2) reads `start[i]`; give it n entries so a harness-side OOB
    // can't be mistaken for a decoder bug. Other codecs ignore it.
    std::vector<int64_t> start((n > 0) ? (size_t)n : 0, 0);
    const int64_t* startp = start.empty() ? nullptr : start.data();

    auto r = lociss_v4::decode_colblock(buf, blen, codec, *type, n, cw, startp);
    (void)r;  // Invalid is fine — sanitizers catch the memory-safety failures.
    return 0;
}
