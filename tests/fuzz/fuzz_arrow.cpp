// libFuzzer harness for the Arrow IPC file and stream readers
// (src/formats/arrow.cpp), each opened from a file as vv opens it
// (source_fuzz_one, src/core/open.cpp). The first byte picks the extension;
// the rest is the file.
//
// ORC is not fuzzed: malformed stripe data crashes liborc's column decoding
// (StringDirectColumnReader sizes a buffer from the LENGTH stream and
// DataBuffer::reserve writes through malloc's unchecked result; apache-orc
// 2.3.1), past anything vv can check before ReadStripe. The footer and stripe
// footers, which liborc parses with protobuf, are checked by vv first
// (orc_check in src/formats/arrow.cpp); adding "orc" to kExt fuzzes them.
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_arrow
//   ./build-fuzz/fuzz_arrow -max_total_time=60 <corpus>
// A seed is a file with its extension's index byte in front: 0 arrow, 1 arrows.
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const char* const kExt[] = {"arrow", "arrows"};
    constexpr size_t kN = sizeof(kExt) / sizeof(kExt[0]);
    if (size < 1) return 0;
    source_fuzz_one(kExt[data[0] % kN], data + 1, size - 1);
    return 0;
}
