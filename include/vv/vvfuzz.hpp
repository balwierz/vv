#pragma once
// Internal declarations exposed for the libFuzzer harnesses in tests/fuzz/.
// NOT part of the public reader surface (that is vvcore.hpp) — these reach into
// the LociSSD v4 "colblock" decoder so it can be fuzzed directly (a pure
// function on a decompressed chunk, so coverage is high and there is no file
// I/O per iteration).
#include <cstddef>
#include <cstdint>
#include <memory>

#include <arrow/array.h>
#include <arrow/result.h>
#include <arrow/type.h>

namespace lociss_v4 {

// Decode one (already zstd-decompressed) LociSSD v4 column chunk into an Arrow
// array. Defined in src/formats/lociss.cpp. `buf`/`blen` is the untrusted chunk; the codec
// decoders must validate every on-disk offset/length against it.
arrow::Result<std::shared_ptr<arrow::Array>>
decode_colblock(const uint8_t* buf, size_t blen, int codec_id,
                const arrow::DataType& type, int64_t n, int cw,
                const int64_t* start);

}  // namespace lociss_v4

// Parse an untrusted .npy buffer and build its table (the NPY header parser plus
// slab_to_arrow / build_1d_table / build_2d_table). Compiled only under VV_FUZZ.
namespace npz { void npy_fuzz_one(const uint8_t* buf, size_t n); }

// Run the JSON pretty printer, the paths writer and the stdin sniff over an
// untrusted buffer; aborts when an invariant breaks (error offset beyond the
// input, pretty print not a fixed point). Compiled only under VV_FUZZ.
namespace vvjson { void fuzz_one(const uint8_t* buf, size_t n); }

// Open an untrusted buffer as a POD5 file and read every tab; VBZ-decode it
// too. Aborts when a built table is not valid. Compiled only under VV_FUZZ.
void pod5_fuzz_one(const uint8_t* buf, size_t n);

// Parse an untrusted buffer as an R .rds / .RData file and build its tables;
// aborts when a built table is not valid. Compiled only under VV_FUZZ.
void rdata_fuzz_one(const uint8_t* buf, size_t n);

// Decode an untrusted LZF stream (HDF5 filter 32000) into `out`. Returns the
// decoded length, or 0 on failure (*out_too_small set if `out` ran out of room).
namespace h5lzf {
size_t decode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len,
              bool* out_too_small);
}
