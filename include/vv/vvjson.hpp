// vvjson.hpp — JSON documents in libvvcore: a strict streaming lexer and the
// writers behind `vv x.json` on a pipe (pretty print) and --json-paths.
//
// The lexer is iterative (an explicit container stack, never recursion), keeps
// numbers as the bytes the file holds, and passes strings through in pieces,
// so memory does not depend on nesting depth or value size. A file may hold
// several top-level values (NDJSON, concatenated JSON).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace arrow { namespace io { class InputStream; } }

namespace vvjson {

// The first error in a document: byte offset (from the start of the decoded
// stream), 1-based line and column, and what was expected. offset < 0 = none.
struct JsonError {
    int64_t     offset = -1;
    int64_t     line   = 0;
    int64_t     col    = 0;
    std::string msg;
    bool ok() const { return offset < 0; }
    // "invalid JSON at byte N (line L, column C): msg"
    std::string describe() const;
};

enum class JsonOut {
    Pretty,   // re-indented, 2 spaces; scalars as written in the file
    Paths,    // one `path = value` line per leaf (and per empty container)
};

// Read every top-level value from `in` and write it to `out`. `lines`: the
// input is NDJSON / JSON Lines, so --json-paths prefixes each record's paths
// with `.[i]`. `color`: colour keys / strings / numbers / literals with the
// active theme. Everything before an error is written; the error is returned.
JsonError write_json_document(arrow::io::InputStream& in, std::FILE* out,
                              JsonOut mode, bool lines, bool color);

}  // namespace vvjson
