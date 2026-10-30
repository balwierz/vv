// libFuzzer harness for the alignment and variant readers: BAM and BCF
// through htslib (src/formats/htslib.cpp), SAM and VCF text through the
// delimited reader, each opened from a file as vv opens it, with the VCF
// sample columns as structs. The first byte picks the extension; the rest
// is the file.
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_hts
//   ./build-fuzz/fuzz_hts -max_total_time=60 <corpus>
// A seed is a file with its extension's index byte in front:
// 0 bam, 1 sam, 2 bcf, 3 vcf.
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const char* const kExt[] = {"bam", "sam", "bcf", "vcf"};
    if (size < 1) return 0;
    source_fuzz_one(kExt[data[0] % 4], data + 1, size - 1);
    return 0;
}
