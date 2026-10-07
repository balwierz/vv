// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// R's serialization format (saveRDS / save / serialize), read into a tree of
// R objects. Internal to src/formats (rserial.cpp parses, rdata.cpp turns the
// tree into tabs).
#pragma once

#include <arrow/io/interfaces.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rser {

// SEXP types (R's Rinternals.h).
enum : int {
    NILSXP = 0, SYMSXP = 1, LISTSXP = 2, CLOSXP = 3, ENVSXP = 4, PROMSXP = 5, LANGSXP = 6,
    SPECIALSXP = 7, BUILTINSXP = 8, CHARSXP = 9, LGLSXP = 10, INTSXP = 13, REALSXP = 14,
    CPLXSXP = 15, STRSXP = 16, DOTSXP = 17, VECSXP = 19, EXPRSXP = 20, BCODESXP = 21,
    EXTPTRSXP = 22, WEAKREFSXP = 23, RAWSXP = 24, S4SXP = 25,
};

constexpr int32_t kNaInteger = INT32_MIN;      // NA_integer_ and NA (logical)

// One R object. Which members are used depends on `type`:
//   LGLSXP / INTSXP   ints              REALSXP           reals
//   CPLXSXP           reals (re, im)    STRSXP            strs (nullopt = NA)
//   RAWSXP            raw               SYMSXP / CHARSXP  text (CHARSXP NA: na)
//   VECSXP / EXPRSXP  items
//   LISTSXP / LANGSXP items + tags (pairlist cells, in order)
//   ENVSXP            items + tags (the bindings; text names a special one)
//   S4SXP             attrs only (its slots)
struct Value {
    int  type = NILSXP;
    bool obj  = false;                 // the OBJECT bit: has a class attribute
    bool na   = false;                 // CHARSXP NA_STRING
    std::vector<int32_t>                    ints;
    std::vector<double>                     reals;
    std::vector<std::optional<std::string>> strs;
    std::vector<uint8_t>                    raw;
    std::string                             text;
    std::vector<const Value*>               items;
    std::vector<std::string>                tags;
    std::vector<std::pair<std::string, const Value*>> attrs;

    const Value* attr(std::string_view name) const {
        for (const auto& [k, v] : attrs) if (k == name) return v;
        return nullptr;
    }
    // The class attribute's strings (empty: none).
    std::vector<std::string> klass() const;
    bool inherits(std::string_view cls) const;
    // Length as R's length(): elements of a vector, cells of a pairlist,
    // bindings of an environment, 0 for NULL.
    int64_t length() const;
    bool is_atomic() const {
        return type == LGLSXP || type == INTSXP || type == REALSXP || type == CPLXSXP ||
               type == STRSXP || type == RAWSXP;
    }
};

// A parsed file: its top-level objects (one, unnamed, for an .rds; the named
// objects of an .RData) in an arena that owns every Value.
struct File {
    std::vector<std::unique_ptr<Value>> arena;
    std::vector<std::pair<std::string, const Value*>> objects;
    bool        rdata   = false;
    int         version = 0;           // serialization format 2 or 3
    std::string format;                // "xdr", "binary", "ascii"
    std::string writer;                // R version that wrote it ("4.4.1")
};

// Parse a decoded (decompressed) stream. "" or an error message.
std::string read(std::shared_ptr<arrow::io::InputStream> in, File* out);

}  // namespace rser
