// libFuzzer harness for Zarr stores: the metadata JSON reader (v2 .zarray /
// .zattrs, v3 zarr.json), the chunk pipeline (blosc, zstd, gzip, zlib, lz4,
// bz2, crc32c, vlen-utf8, byte order, transpose), shard indexes, and the
// AnnData / generic tab code reading through the Store interface. The input
// is an in-memory store: [u16 key length][key][u32 value length][value]
// records; tests/fuzz/zarr_seeds.py turns the tests/data/*.zarr.zip fixtures
// into seeds. Every built table is checked with ValidateFull().
//
//   cmake -S . -B build-fuzz -DVV_BUILD_FUZZERS=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz -j"$(nproc)" --target fuzz_zarr
//   python3 tests/fuzz/zarr_seeds.py corpus/ && ./build-fuzz/fuzz_zarr -max_len=131072 corpus/
#include "vv/vvfuzz.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    h5v::zarr_fuzz_one(data, size);
    return 0;
}
