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
// array. Defined in main.cpp. `buf`/`blen` is the untrusted chunk; the codec
// decoders must validate every on-disk offset/length against it.
arrow::Result<std::shared_ptr<arrow::Array>>
decode_colblock(const uint8_t* buf, size_t blen, int codec_id,
                const arrow::DataType& type, int64_t n, int cw,
                const int64_t* start);

}  // namespace lociss_v4
