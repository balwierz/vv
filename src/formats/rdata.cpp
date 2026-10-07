// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// R data files: .rds (one object) and .RData / .rda (several), parsed by
// rserial.cpp into a tree of R objects and shown as tabs:
//   - a data frame, an atomic vector or a matrix: one table (factors as
//     categorical columns, Date / POSIXct as date32 / timestamp, bit64
//     integer64 as int64, character row names as an `index` column);
//   - a list, an S4 object (Seurat, SingleCellExperiment, ...) or an
//     environment: a structure tab — every element / slot with its class,
//     type, size and first values — plus a tab per table-like part, named by
//     its R path (`@meta.data`, `@assays$RNA@layers$counts`, `$df`).
// Bioconductor DFrames become tables, Matrix-package sparse matrices
// (dgCMatrix, dgTMatrix, dgRMatrix and their logical / pattern kinds) a
// preview of their first rows × columns (with --matrix long: every stored
// value as row, col, value), dense dgeMatrix a matrix. Seurat v5 layers and
// SummarizedExperiment assays without dimnames take them from the object
// (the assay's cells / features maps; NAMES and colData row names).

#include "internal.hpp"
#include "rserial.hpp"

using rser::Value;

namespace {

constexpr int64_t kPreviewRows  = 1000;    // sparse matrix preview: rows ...
constexpr int64_t kPreviewCols  = 200;     // ... and columns
constexpr int64_t kMaxMatrixCols = 4096;   // dense matrix columns built
constexpr size_t  kMaxTabs      = 1000;
constexpr size_t  kMaxStructRows = 200000;
constexpr int     kMaxWalkDepth = 24;
constexpr int64_t kLongChunk    = 1 << 20;

bool is_na_real(double v) {
    if (!std::isnan(v)) return false;
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    return (bits & 0xffffffffu) == 1954;          // R's NA_real_ (NaN stays NaN)
}

std::string fmt_double(double v) {
    if (is_na_real(v)) return "NA";
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Inf" : "-Inf";
    char b[32];
    std::snprintf(b, sizeof b, "%.15g", v);
    return b;
}

const std::vector<std::string>* strs_or_null(const Value* v, std::vector<std::string>* tmp) {
    if (!v || v->type != rser::STRSXP) return nullptr;
    tmp->clear();
    for (const auto& s : v->strs) tmp->push_back(s ? *s : "NA");
    return tmp;
}
std::vector<std::string> str_vec(const Value* v) {
    std::vector<std::string> out;
    if (v && v->type == rser::STRSXP)
        for (const auto& s : v->strs) out.push_back(s ? *s : "NA");
    return out;
}

// dim attribute (or a Matrix object's Dim slot) when it has two entries.
bool dims2(const Value* d, int64_t* nr, int64_t* nc) {
    if (!d || d->type != rser::INTSXP || d->ints.size() != 2 || d->ints[0] < 0 || d->ints[1] < 0) return false;
    *nr = d->ints[0];
    *nc = d->ints[1];
    return true;
}
// dimnames (list of two, each a character vector or NULL).
void dimnames(const Value* dn, std::vector<std::string>* rn, std::vector<std::string>* cn) {
    if (!dn || dn->type != rser::VECSXP || dn->items.size() != 2) return;
    *rn = str_vec(dn->items[0]);
    *cn = str_vec(dn->items[1]);
}

const Value* slot(const Value* v, std::string_view name) { return v ? v->attr(name) : nullptr; }

bool is_sparse_matrix(const Value* v) {
    if (!v || v->type != rser::S4SXP) return false;
    int64_t nr, nc;
    if (!dims2(slot(v, "Dim"), &nr, &nc)) return false;
    const bool i = slot(v, "i"), j = slot(v, "j"), p = slot(v, "p");
    return (i && p) || (j && p) || (i && j);
}
bool is_dense_matrix_s4(const Value* v) {      // dgeMatrix and friends
    if (!v || v->type != rser::S4SXP || is_sparse_matrix(v)) return false;
    int64_t nr, nc;
    const Value* x = slot(v, "x");
    return dims2(slot(v, "Dim"), &nr, &nc) && x && x->is_atomic() && x->length() == nr * nc;
}
bool is_dframe(const Value* v) {
    return v && v->type == rser::S4SXP && slot(v, "listData") && slot(v, "nrows") &&
           (v->inherits("DFrame") || v->inherits("DataFrame"));
}
bool is_matrix(const Value* v) {
    int64_t nr, nc;
    return v && v->is_atomic() && dims2(v->attr("dim"), &nr, &nc) && v->length() == nr * nc;
}
bool is_data_frame(const Value* v) { return v && v->type == rser::VECSXP && v->inherits("data.frame"); }
bool is_rle(const Value* v) {
    return v && v->type == rser::S4SXP && v->inherits("Rle") && slot(v, "values") && slot(v, "lengths");
}
bool is_granges(const Value* v) {
    return v && v->type == rser::S4SXP && slot(v, "seqnames") && slot(v, "ranges") && slot(v, "strand");
}
// A data frame / DFrame column that is itself a table (shown as its own tab).
bool nested_column(const Value* c) {
    return is_matrix(c) || is_data_frame(c) || is_dframe(c) || is_sparse_matrix(c) ||
           (c->type == rser::S4SXP && !is_rle(c));
}
bool is_version(const Value* v) {
    return v && v->type == rser::VECSXP && (v->inherits("package_version") || v->inherits("numeric_version"));
}
std::string version_text(const Value* v) {
    std::string out;
    for (const Value* e : v->items) {
        if (!out.empty()) out += ", ";
        std::string one;
        if (e->type == rser::INTSXP)
            for (int32_t x : e->ints) one += (one.empty() ? "" : ".") + std::to_string(x);
        out += one;
    }
    return out;
}

std::string type_name(const Value* v) {
    switch (v->type) {
        case rser::NILSXP: return "NULL";       case rser::SYMSXP: return "symbol";
        case rser::LISTSXP: return "pairlist";  case rser::CLOSXP: return "closure";
        case rser::ENVSXP: return "environment"; case rser::PROMSXP: return "promise";
        case rser::LANGSXP: return "language";  case rser::SPECIALSXP: return "special";
        case rser::BUILTINSXP: return "builtin"; case rser::CHARSXP: return "char";
        case rser::LGLSXP: return "logical";    case rser::INTSXP: return "integer";
        case rser::REALSXP: return "double";    case rser::CPLXSXP: return "complex";
        case rser::STRSXP: return "character";  case rser::VECSXP: return "list";
        case rser::EXPRSXP: return "expression"; case rser::BCODESXP: return "bytecode";
        case rser::EXTPTRSXP: return "externalptr"; case rser::WEAKREFSXP: return "weakref";
        case rser::RAWSXP: return "raw";        case rser::S4SXP: return "S4";
        default: return "?";
    }
}
// class(x) for display: the class attribute (S4: with its package), or the
// implicit class.
std::string class_label(const Value* v) {
    auto k = v->klass();
    if (!k.empty()) {
        std::string s;
        for (const auto& c : k) s += (s.empty() ? "" : ", ") + c;
        if (const Value* c = v->attr("class"))
            if (const Value* pkg = c->attr("package"); pkg && pkg->type == rser::STRSXP && !pkg->strs.empty() &&
                                                       pkg->strs[0])
                s += " (" + *pkg->strs[0] + ")";
        return s;
    }
    if (is_matrix(v)) return "matrix";
    if (v->type == rser::CLOSXP || v->type == rser::BUILTINSXP || v->type == rser::SPECIALSXP) return "function";
    if (v->type == rser::REALSXP) return "numeric";
    return type_name(v);
}

// ── R vectors as Arrow arrays ───────────────────────────────────────────────

std::string cell_text(const Value* v, int64_t k) {
    switch (v->type) {
        case rser::LGLSXP: { const int32_t x = v->ints[(size_t)k]; return x == rser::kNaInteger ? "NA" : x ? "TRUE" : "FALSE"; }
        case rser::INTSXP: { const int32_t x = v->ints[(size_t)k]; return x == rser::kNaInteger ? "NA" : std::to_string(x); }
        case rser::REALSXP: return fmt_double(v->reals[(size_t)k]);
        case rser::CPLXSXP: {
            const double re = v->reals[2 * (size_t)k], im = v->reals[2 * (size_t)k + 1];
            return fmt_double(re) + (im < 0 ? "-" : "+") + fmt_double(std::fabs(im)) + "i";
        }
        case rser::STRSXP: { const auto& s = v->strs[(size_t)k]; return s ? *s : "NA"; }
        case rser::RAWSXP: { char b[4]; std::snprintf(b, sizeof b, "%02x", v->raw[(size_t)k]); return b; }
        default: return "";
    }
}

// A short display of a value's first elements.
std::string preview(const Value* v, size_t max_chars = 80) {
    if (is_version(v)) return version_text(v);
    if (v->type == rser::SYMSXP) return v->text;
    if (v->type == rser::ENVSXP && !v->text.empty()) return "<" + v->text + ">";
    if (!v->is_atomic()) return "";
    const Value* lv = v->attr("levels");
    std::string s;
    const int64_t n = v->length();
    for (int64_t k = 0; k < n; ++k) {
        std::string t;
        if (lv && v->type == rser::INTSXP && lv->type == rser::STRSXP) {
            const int32_t c = v->ints[(size_t)k];
            t = c >= 1 && c <= (int32_t)lv->strs.size() && lv->strs[(size_t)c - 1] ? *lv->strs[(size_t)c - 1] : "NA";
        } else {
            t = cell_text(v, k);
        }
        if (!s.empty()) s += ", ";
        if (s.size() + t.size() > max_chars) { s += "\xe2\x80\xa6"; break; }
        s += t;
    }
    return s;
}

arrow::Result<std::shared_ptr<arrow::Array>> strings_array(const std::vector<std::string>& v, size_t n) {
    arrow::StringBuilder b;
    for (size_t k = 0; k < n; ++k) ARROW_RETURN_NOT_OK(k < v.size() ? b.Append(v[k]) : b.AppendNull());
    std::shared_ptr<arrow::Array> a;
    ARROW_RETURN_NOT_OK(b.Finish(&a));
    return a;
}

// Elements [first, first + n) of an R vector as an Arrow array.
arrow::Result<std::shared_ptr<arrow::Array>> to_array(const Value* v, int64_t first, int64_t n) {
    std::shared_ptr<arrow::Array> out;
    const int64_t len = v->length();
    if (first < 0 || n < 0 || first + n > len) return arrow::Status::Invalid("R vector shorter than its table");
    if (is_rle(v)) return arrow::Status::Invalid("Rle");          // expanded by the caller
    const Value* lv = v->attr("levels");
    if (v->type == rser::INTSXP && lv && lv->type == rser::STRSXP && (v->inherits("factor") || v->obj)) {
        arrow::Int32Builder b;
        const int32_t nl = (int32_t)lv->strs.size();
        for (int64_t k = first; k < first + n; ++k) {
            const int32_t c = v->ints[(size_t)k];          // 1-based codes; out of range → null
            ARROW_RETURN_NOT_OK(c == rser::kNaInteger || c < 1 || c > nl ? b.AppendNull() : b.Append(c - 1));
        }
        ARROW_RETURN_NOT_OK(b.Finish(&out));
        std::vector<std::string> levels = str_vec(lv);
        ARROW_ASSIGN_OR_RAISE(auto dict, strings_array(levels, levels.size()));
        return arrow::DictionaryArray::FromArrays(arrow::dictionary(arrow::int32(), arrow::utf8()), out, dict);
    }
    if (v->inherits("Date") && (v->type == rser::REALSXP || v->type == rser::INTSXP)) {
        arrow::Date32Builder b;
        for (int64_t k = first; k < first + n; ++k) {
            const double d = v->type == rser::REALSXP ? v->reals[(size_t)k]
                           : v->ints[(size_t)k] == rser::kNaInteger ? NAN : (double)v->ints[(size_t)k];
            // days since 1970; outside the int32 range (or NaN / NA) is null
            ARROW_RETURN_NOT_OK(std::fabs(d) < 2147483647.0 ? b.Append((int32_t)std::floor(d)) : b.AppendNull());
        }
        ARROW_RETURN_NOT_OK(b.Finish(&out));
        return out;
    }
    if (v->inherits("POSIXct") && (v->type == rser::REALSXP || v->type == rser::INTSXP)) {
        arrow::TimestampBuilder b(arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"), arrow::default_memory_pool());
        for (int64_t k = first; k < first + n; ++k) {
            const double d = v->type == rser::REALSXP ? v->reals[(size_t)k]
                           : v->ints[(size_t)k] == rser::kNaInteger ? NAN : (double)v->ints[(size_t)k];
            // seconds since 1970 as microseconds; beyond int64 (or NaN / NA) is null
            ARROW_RETURN_NOT_OK(std::fabs(d) < 9.2e12 ? b.Append((int64_t)std::llround(d * 1e6)) : b.AppendNull());
        }
        ARROW_RETURN_NOT_OK(b.Finish(&out));
        return out;
    }
    switch (v->type) {
        case rser::LGLSXP: {
            arrow::BooleanBuilder b;
            for (int64_t k = first; k < first + n; ++k) {
                const int32_t x = v->ints[(size_t)k];
                ARROW_RETURN_NOT_OK(x == rser::kNaInteger ? b.AppendNull() : b.Append(x != 0));
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        case rser::INTSXP: {
            arrow::Int32Builder b;
            for (int64_t k = first; k < first + n; ++k) {
                const int32_t x = v->ints[(size_t)k];
                ARROW_RETURN_NOT_OK(x == rser::kNaInteger ? b.AppendNull() : b.Append(x));
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        case rser::REALSXP: {
            if (v->inherits("integer64")) {               // bit64: int64 bits in a double
                arrow::Int64Builder b;
                for (int64_t k = first; k < first + n; ++k) {
                    int64_t x;
                    std::memcpy(&x, &v->reals[(size_t)k], 8);
                    ARROW_RETURN_NOT_OK(x == INT64_MIN ? b.AppendNull() : b.Append(x));
                }
                ARROW_RETURN_NOT_OK(b.Finish(&out));
                return out;
            }
            arrow::DoubleBuilder b;
            for (int64_t k = first; k < first + n; ++k) {
                const double x = v->reals[(size_t)k];
                ARROW_RETURN_NOT_OK(is_na_real(x) ? b.AppendNull() : b.Append(x));
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        case rser::STRSXP: {
            arrow::StringBuilder b;
            for (int64_t k = first; k < first + n; ++k) {
                const auto& s = v->strs[(size_t)k];
                ARROW_RETURN_NOT_OK(s ? b.Append(*s) : b.AppendNull());
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        case rser::RAWSXP: {
            arrow::UInt8Builder b;
            for (int64_t k = first; k < first + n; ++k) ARROW_RETURN_NOT_OK(b.Append(v->raw[(size_t)k]));
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        case rser::CPLXSXP: {
            arrow::StringBuilder b;
            for (int64_t k = first; k < first + n; ++k) ARROW_RETURN_NOT_OK(b.Append(cell_text(v, k)));
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        case rser::VECSXP: {                               // a list column: each element shown as text
            arrow::StringBuilder b;
            for (int64_t k = first; k < first + n; ++k) {
                const Value* e = v->items[(size_t)k];
                ARROW_RETURN_NOT_OK(e->type == rser::NILSXP ? b.AppendNull() : b.Append(preview(e, 200)));
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        default: return arrow::Status::NotImplemented("R ", type_name(v), " column");
    }
}

// An S4Vectors Rle expanded (values repeated lengths times), as a plain vector.
const Value* expand_rle(const Value* v, std::vector<std::unique_ptr<Value>>* own) {
    const Value* vals = slot(v, "values");
    const Value* lens = slot(v, "lengths");
    if (!vals || !lens || lens->type != rser::INTSXP || vals->length() != (int64_t)lens->ints.size()) return nullptr;
    auto e = std::make_unique<Value>();
    e->type = vals->type;
    e->attrs = vals->attrs;
    int64_t total = 0;
    for (int32_t l : lens->ints) { if (l < 0) return nullptr; total += l; }
    if (total > (int64_t(1) << 31)) return nullptr;
    for (size_t k = 0; k < lens->ints.size(); ++k)
        for (int32_t r = 0; r < lens->ints[k]; ++r) {
            switch (vals->type) {
                case rser::LGLSXP: case rser::INTSXP: e->ints.push_back(vals->ints[k]); break;
                case rser::REALSXP: e->reals.push_back(vals->reals[k]); break;
                case rser::STRSXP: e->strs.push_back(vals->strs[k]); break;
                default: return nullptr;
            }
        }
    own->push_back(std::move(e));
    return own->back().get();
}

// ── tabs ─────────────────────────────────────────────────────────────────────

struct Tab {
    enum Kind { Frame, DFrame, Matrix, Sparse, Dense4, Vector, Structure, GRanges } kind;
    std::string label;
    const Value* v = nullptr;
    std::string object;                  // the top-level object's name (.RData), for a vector's column
    std::vector<std::string> row_names, col_names;   // names supplied by the containing object
    std::shared_ptr<arrow::Table> structure;          // Structure tabs: built during the walk
    std::string what;                                 // the structure's object class
};

struct State {
    rser::File file;
    std::vector<Tab> tabs;
    std::string path;
    bool rds = true, matrix_long = false;
    size_t skipped_tabs = 0;
};

// Build a data frame / DFrame table: columns of `cols` (named by `names`),
// `rows` rows, row names as an index column.
arrow::Result<std::shared_ptr<arrow::Table>> frame_table(const std::vector<const Value*>& cols,
                                                         const std::vector<std::string>& names, int64_t rows,
                                                         const std::vector<std::string>& row_names,
                                                         std::string* note) {
    arrow::FieldVector fields;
    arrow::ArrayVector arrays;
    if (!row_names.empty() && (int64_t)row_names.size() == rows) {
        fields.push_back(arrow::field("index", arrow::utf8()));
        ARROW_ASSIGN_OR_RAISE(auto a, strings_array(row_names, (size_t)rows));
        arrays.push_back(a);
    }
    std::vector<std::unique_ptr<Value>> own;
    std::vector<std::string> dropped;
    for (size_t k = 0; k < cols.size(); ++k) {
        const Value* c = cols[k];
        const std::string nm = k < names.size() && !names[k].empty() ? names[k] : "V" + std::to_string(k + 1);
        if (is_rle(c)) c = expand_rle(c, &own);
        if (!c || is_matrix(c) || is_data_frame(c) || is_dframe(c) || c->type == rser::S4SXP ||
            (!c->is_atomic() && c->type != rser::VECSXP) || c->length() != rows) {
            dropped.push_back(nm);
            continue;
        }
        auto a = to_array(c, 0, rows);
        if (!a.ok()) { dropped.push_back(nm); continue; }
        fields.push_back(arrow::field(nm, (*a)->type()));
        arrays.push_back(*a);
    }
    if (!dropped.empty()) {
        std::string d;
        for (const auto& s : dropped) d += (d.empty() ? "" : ", ") + s;
        *note = "not shown (nested / other length): " + d;
    }
    return arrow::Table::Make(arrow::schema(fields), arrays, rows);
}

int64_t df_rows(const Value* df, std::vector<std::string>* row_names) {
    const Value* rn = df->attr("row.names");
    if (rn && rn->type == rser::INTSXP) {
        if (rn->ints.size() == 2 && rn->ints[0] == rser::kNaInteger) return std::abs((int64_t)rn->ints[1]);
        return (int64_t)rn->ints.size();
    }
    if (rn && rn->type == rser::STRSXP) {
        *row_names = str_vec(rn);
        return (int64_t)rn->strs.size();
    }
    int64_t n = 0;
    for (const Value* c : df->items) n = std::max(n, c->length());
    return n;
}

// A dense R matrix (column-major values) as a table: row names as `index`,
// at most kMaxMatrixCols columns.
arrow::Result<std::shared_ptr<arrow::Table>> matrix_table(const Value* x, int64_t nr, int64_t nc,
                                                          std::vector<std::string> rn, std::vector<std::string> cn,
                                                          std::string* note) {
    arrow::FieldVector fields;
    arrow::ArrayVector arrays;
    if ((int64_t)rn.size() == nr && nr > 0) {
        fields.push_back(arrow::field("index", arrow::utf8()));
        ARROW_ASSIGN_OR_RAISE(auto a, strings_array(rn, (size_t)nr));
        arrays.push_back(a);
    }
    const int64_t shown = std::min(nc, kMaxMatrixCols);
    for (int64_t c = 0; c < shown; ++c) {
        ARROW_ASSIGN_OR_RAISE(auto a, to_array(x, c * nr, nr));
        fields.push_back(arrow::field((int64_t)cn.size() == nc && !cn[(size_t)c].empty() ? cn[(size_t)c]
                                                                                       : "V" + std::to_string(c + 1),
                                      a->type()));
        arrays.push_back(a);
    }
    if (shown < nc) *note = "showing the first " + std::to_string(shown) + " of " + std::to_string(nc) + " columns";
    return arrow::Table::Make(arrow::schema(fields), arrays, nr);
}

// A Matrix-package sparse matrix: its stored entries, whichever layout.
struct Sparse {
    char layout = 'C';                 // C (i, p), R (j, p), T (i, j)
    int64_t nr = 0, nc = 0;
    const Value *i = nullptr, *j = nullptr, *p = nullptr, *x = nullptr;
    std::string cls;
    bool logical = false;              // l*/n* kinds: TRUE / pattern
    int64_t nnz() const {
        const Value* idx = layout == 'R' ? j : i;
        return idx ? (int64_t)idx->ints.size() : 0;
    }
    double value(int64_t k) const {
        if (!x) return 1.0;                                   // pattern matrix
        if (x->type == rser::REALSXP) return (size_t)k < x->reals.size() ? x->reals[(size_t)k] : NAN;
        if (x->type == rser::LGLSXP || x->type == rser::INTSXP) {
            if ((size_t)k >= x->ints.size()) return NAN;
            const int32_t v = x->ints[(size_t)k];
            return v == rser::kNaInteger ? NAN : (double)v;
        }
        return NAN;
    }
    static bool make(const Value* v, Sparse* s) {
        if (!dims2(slot(v, "Dim"), &s->nr, &s->nc)) return false;
        s->i = slot(v, "i"); s->j = slot(v, "j"); s->p = slot(v, "p"); s->x = slot(v, "x");
        auto ok = [](const Value* a) { return a && a->type == rser::INTSXP; };
        if (ok(s->i) && ok(s->p)) s->layout = 'C';
        else if (ok(s->j) && ok(s->p)) s->layout = 'R';
        else if (ok(s->i) && ok(s->j)) s->layout = 'T';
        else return false;
        if (s->x && !s->x->is_atomic()) s->x = nullptr;
        auto k = v->klass();
        s->cls = k.empty() ? "sparse matrix" : k[0];
        s->logical = !s->cls.empty() && (s->cls[0] == 'l' || s->cls[0] == 'n');
        return true;
    }
    // Visit stored entries (row, col, index into x), skipping out-of-range
    // indices (the file is untrusted). `f` returns false to stop.
    template <typename F> void each(F f, int64_t max_row = INT64_MAX, int64_t max_col = INT64_MAX) const {
        if (layout == 'T') {
            const size_t n = std::min(i->ints.size(), j->ints.size());
            for (size_t k = 0; k < n; ++k) {
                const int64_t r = i->ints[k], c = j->ints[k];
                if (r < 0 || r >= nr || c < 0 || c >= nc || r >= max_row || c >= max_col) continue;
                if (!f(r, c, (int64_t)k)) return;
            }
            return;
        }
        const Value* idx = layout == 'C' ? i : j;
        const int64_t outer = layout == 'C' ? nc : nr, outer_max = layout == 'C' ? max_col : max_row;
        const int64_t inner_n = layout == 'C' ? nr : nc, inner_max = layout == 'C' ? max_row : max_col;
        const int64_t nidx = (int64_t)idx->ints.size();
        for (int64_t o = 0; o < outer && o < outer_max && o + 1 < (int64_t)p->ints.size(); ++o) {
            const int64_t lo = std::clamp<int64_t>(p->ints[(size_t)o], 0, nidx);
            const int64_t hi = std::clamp<int64_t>(p->ints[(size_t)o + 1], lo, nidx);
            for (int64_t k = lo; k < hi; ++k) {
                const int64_t in = idx->ints[(size_t)k];
                if (in < 0 || in >= inner_n) continue;
                if (in >= inner_max) break;                   // sorted within a column / row
                if (!f(layout == 'C' ? in : o, layout == 'C' ? o : in, k)) return;
            }
        }
    }
};

arrow::Result<std::shared_ptr<arrow::Table>> sparse_preview(const Sparse& s, const std::vector<std::string>& rn,
                                                            const std::vector<std::string>& cn) {
    const int64_t rows = std::min(s.nr, kPreviewRows), cols = std::min(s.nc, kPreviewCols);
    std::vector<double> dense((size_t)(rows * cols), 0.0);
    s.each([&](int64_t r, int64_t c, int64_t k) {
        dense[(size_t)(c * rows + r)] += s.value(k);
        return true;
    }, rows, cols);
    arrow::FieldVector fields;
    arrow::ArrayVector arrays;
    if ((int64_t)rn.size() == s.nr && s.nr > 0) {
        fields.push_back(arrow::field("index", arrow::utf8()));
        ARROW_ASSIGN_OR_RAISE(auto a, strings_array(rn, (size_t)rows));
        arrays.push_back(a);
    }
    for (int64_t c = 0; c < cols; ++c) {
        std::shared_ptr<arrow::Array> a;
        if (s.logical) {
            arrow::BooleanBuilder b;
            for (int64_t r = 0; r < rows; ++r) {
                const double d = dense[(size_t)(c * rows + r)];
                ARROW_RETURN_NOT_OK(std::isnan(d) ? b.AppendNull() : b.Append(d != 0));
            }
            ARROW_RETURN_NOT_OK(b.Finish(&a));
        } else {
            arrow::DoubleBuilder b;
            for (int64_t r = 0; r < rows; ++r) {
                const double d = dense[(size_t)(c * rows + r)];
                ARROW_RETURN_NOT_OK(std::isnan(d) ? b.AppendNull() : b.Append(d));
            }
            ARROW_RETURN_NOT_OK(b.Finish(&a));
        }
        fields.push_back(arrow::field((int64_t)cn.size() == s.nc && !cn[(size_t)c].empty() ? cn[(size_t)c]
                                                                                         : "V" + std::to_string(c + 1),
                                      a->type()));
        arrays.push_back(a);
    }
    return arrow::Table::Make(arrow::schema(fields), arrays, rows);
}

std::string shape(int64_t nr, int64_t nc) { return std::to_string(nr) + " \xc3\x97 " + std::to_string(nc); }

// The table of one tab, and its footer description.
arrow::Result<std::shared_ptr<arrow::Table>> build(const State& st, const Tab& t, std::string* kind,
                                                   PreviewLimit* limit) {
    const Value* v = t.v;
    std::string note;
    std::shared_ptr<arrow::Table> out;
    switch (t.kind) {
        case Tab::Structure:
            *kind = "structure of " + t.what;
            return t.structure;
        case Tab::Frame: {
            std::vector<std::string> rn;
            const int64_t rows = df_rows(v, &rn);
            if (rn.empty()) rn = t.row_names;
            ARROW_ASSIGN_OR_RAISE(out, frame_table(v->items, str_vec(v->attr("names")), rows, rn, &note));
            *kind = "data frame " + shape(rows, (int64_t)v->items.size());
            break;
        }
        case Tab::DFrame: {
            const Value* ld = slot(v, "listData");
            const Value* nrows = slot(v, "nrows");
            int64_t rows = nrows && nrows->type == rser::INTSXP && !nrows->ints.empty() ? nrows->ints[0] : 0;
            std::vector<std::string> rn = str_vec(slot(v, "rownames"));
            if (rn.empty()) rn = t.row_names;
            std::vector<const Value*> cols = ld && ld->type == rser::VECSXP ? ld->items : std::vector<const Value*>{};
            ARROW_ASSIGN_OR_RAISE(out, frame_table(cols, str_vec(ld ? ld->attr("names") : nullptr), rows, rn, &note));
            *kind = class_label(v) + " " + shape(rows, (int64_t)cols.size());
            break;
        }
        case Tab::Matrix: case Tab::Dense4: {
            const Value* x = t.kind == Tab::Matrix ? v : slot(v, "x");
            int64_t nr = 0, nc = 0;
            dims2(t.kind == Tab::Matrix ? v->attr("dim") : slot(v, "Dim"), &nr, &nc);
            std::vector<std::string> rn = t.row_names, cn = t.col_names;
            if (rn.empty() && cn.empty())
                dimnames(t.kind == Tab::Matrix ? v->attr("dimnames") : slot(v, "Dimnames"), &rn, &cn);
            ARROW_ASSIGN_OR_RAISE(out, matrix_table(x, nr, nc, rn, cn, &note));
            *kind = (t.kind == Tab::Matrix ? std::string("matrix ") : class_label(v) + " ") + shape(nr, nc);
            if (limit && nc > kMaxMatrixCols) *limit = PreviewLimit{nr, nr, out->num_columns(), nc};
            break;
        }
        case Tab::Sparse: {
            Sparse s;
            if (!Sparse::make(v, &s)) return arrow::Status::Invalid("not a sparse matrix");
            std::vector<std::string> rn = t.row_names, cn = t.col_names;
            if (rn.empty() && cn.empty()) dimnames(slot(v, "Dimnames"), &rn, &cn);
            ARROW_ASSIGN_OR_RAISE(out, sparse_preview(s, rn, cn));
            const double dens = s.nr && s.nc ? 100.0 * (double)s.nnz() / ((double)s.nr * (double)s.nc) : 0;
            char d[32];
            std::snprintf(d, sizeof d, "%.3g%%", dens);
            *kind = s.cls + " " + shape(s.nr, s.nc) + ", " + std::to_string(s.nnz()) + " stored (" + d + ")";
            if (s.nr > kPreviewRows || s.nc > kPreviewCols) {
                note = "preview: first " + std::to_string(std::min(s.nr, kPreviewRows)) + " rows \xc3\x97 " +
                       std::to_string(std::min(s.nc, kPreviewCols)) + " columns; --matrix long lists every stored value";
                if (limit) *limit = PreviewLimit{std::min(s.nr, kPreviewRows), s.nr,
                                                 out->num_columns(), s.nc};
            }
            break;
        }
        case Tab::GRanges: {
            // seqnames / strand are Rle; ranges an IRanges (start, width, NAMES);
            // elementMetadata the metadata columns.
            std::vector<std::unique_ptr<Value>> own;
            const Value* seqn = slot(v, "seqnames");
            const Value* strand = slot(v, "strand");
            if (is_rle(seqn)) seqn = expand_rle(seqn, &own);
            if (is_rle(strand)) strand = expand_rle(strand, &own);
            const Value* rg = slot(v, "ranges");
            const Value* start = slot(rg, "start");
            const Value* width = slot(rg, "width");
            if (!seqn || !strand || !start || !width || start->type != rser::INTSXP || width->type != rser::INTSXP)
                return arrow::Status::Invalid("unreadable GRanges");
            const int64_t n = (int64_t)start->ints.size();
            auto endv = std::make_unique<Value>();
            endv->type = rser::INTSXP;
            for (int64_t k = 0; k < n && k < (int64_t)width->ints.size(); ++k)
                endv->ints.push_back(start->ints[(size_t)k] == rser::kNaInteger ? rser::kNaInteger
                                                                               : start->ints[(size_t)k] + width->ints[(size_t)k] - 1);
            std::vector<const Value*> cols{seqn, start, endv.get(), width, strand};
            std::vector<std::string> names{"seqnames", "start", "end", "width", "strand"};
            const Value* md = slot(v, "elementMetadata");
            if (const Value* ld = slot(md, "listData"); ld && ld->type == rser::VECSXP) {
                auto mn = str_vec(ld->attr("names"));
                for (size_t k = 0; k < ld->items.size(); ++k) {
                    cols.push_back(ld->items[k]);
                    names.push_back(k < mn.size() ? mn[k] : "");
                }
            }
            ARROW_ASSIGN_OR_RAISE(out, frame_table(cols, names, n, str_vec(slot(rg, "NAMES")), &note));
            *kind = class_label(v) + ", " + std::to_string(n) + " ranges (1-based, closed)";
            break;
        }
        case Tab::Vector: {
            std::vector<std::string> names = str_vec(v->attr("names"));
            std::vector<const Value*> cols{v};
            const std::string col = !t.object.empty() && t.label == t.object ? t.object : "value";
            ARROW_ASSIGN_OR_RAISE(out, frame_table(cols, {col}, v->length(), names, &note));
            *kind = (v->inherits("factor") ? std::string("factor") : class_label(v)) + " vector, length " +
                    std::to_string(v->length());
            break;
        }
    }
    if (!note.empty()) *kind += "  |  " + note;
    (void)st;
    return out;
}

// ── walking an object into tabs ─────────────────────────────────────────────

struct Walker {
    State& st;
    std::string top;                     // .RData object name ("" for .rds)
    std::vector<std::string> spath, sclass, stype, ssize, sprev;
    std::set<const Value*> seen_env;

    void row(const std::string& path, const Value* v, const std::string& note) {
        if (spath.size() >= kMaxStructRows) return;
        spath.push_back(path.empty() ? "." : path);
        sclass.push_back(class_label(v));
        stype.push_back(type_name(v));
        int64_t nr, nc;
        if (dims2(v->attr("dim"), &nr, &nc) || (v->type == rser::S4SXP && dims2(slot(v, "Dim"), &nr, &nc)))
            ssize.push_back(shape(nr, nc));
        else if (is_data_frame(v)) {
            std::vector<std::string> rn;
            ssize.push_back(shape(df_rows(v, &rn), (int64_t)v->items.size()));
        } else if (is_dframe(v)) {
            const Value* n = slot(v, "nrows");
            const Value* ld = slot(v, "listData");
            ssize.push_back(shape(n && !n->ints.empty() ? n->ints[0] : 0, ld ? ld->length() : 0));
        } else if (v->type == rser::S4SXP || v->type == rser::CLOSXP || v->type == rser::LANGSXP) {
            ssize.push_back("");
        } else {
            ssize.push_back(std::to_string(v->length()));
        }
        sprev.push_back(note.empty() ? preview(v) : note);
    }
    std::string label_of(const std::string& path) const { return top.empty() ? path : top + path; }
    bool add_tab(Tab t) {
        if (st.tabs.size() >= kMaxTabs) { ++st.skipped_tabs; return false; }
        st.tabs.push_back(std::move(t));
        return true;
    }
    static std::string child_path(const std::string& parent, const std::string& name, size_t k) {
        if (name.empty()) return parent + "[[" + std::to_string(k + 1) + "]]";
        bool plain = std::isalpha((unsigned char)name[0]) || name[0] == '.';
        for (char c : name) plain = plain && (std::isalnum((unsigned char)c) || c == '.' || c == '_');
        return parent + (plain ? "$" + name : "[[\"" + name + "\"]]");
    }

    // Names for a matrix from its container (Seurat Assay5 maps; a
    // SummarizedExperiment's NAMES / colData row names).
    struct Names { std::vector<std::string> rows, cols; };

    // The tab kind of a table-like value, or -1.
    static int tab_kind(const Value* v, bool toplevel) {
        int64_t nr = 0, nc = 0;
        if (is_data_frame(v)) return v->items.empty() && !toplevel ? -2 : Tab::Frame;
        if (is_dframe(v)) {
            const Value* ld = slot(v, "listData");
            return (!ld || ld->length() == 0) && !toplevel ? -2 : Tab::DFrame;
        }
        if (is_granges(v)) return Tab::GRanges;
        if (is_sparse_matrix(v)) return Tab::Sparse;
        if (is_dense_matrix_s4(v)) return Tab::Dense4;
        if (is_matrix(v)) {
            dims2(v->attr("dim"), &nr, &nc);
            if (!toplevel && (nr == 0 || nc == 0 || v->inherits("LogMap"))) return -2;   // empty; Seurat's maps
            return Tab::Matrix;
        }
        if (v->is_atomic() && (v->length() > 1 || toplevel)) return Tab::Vector;
        return -1;
    }

    void walk(const Value* v, const std::string& path, int depth, const Names* names = nullptr) {
        const int kind = tab_kind(v, false);
        if (kind == -2 || is_version(v)) { row(path, v, ""); return; }   // listed, no tab
        // GRangesList and the other S4Vectors compressed lists: listed only.
        if (v->type == rser::S4SXP && slot(v, "unlistData") && slot(v, "partitioning")) {
            const Value* pt = slot(v, "partitioning");
            const Value* end = slot(pt, "end");
            row(path, v, std::to_string(end ? end->length() : 0) + " elements (not expanded)");
            // its metadata columns, one row per element (a
            // RangedSummarizedExperiment's rowData)
            if (const Value* md = slot(v, "elementMetadata"); md && depth < kMaxWalkDepth)
                walk(md, path + "@elementMetadata", depth + 1, names);
            return;
        }
        if (kind == Tab::Frame || kind == Tab::DFrame) {
            // A frame holding only nested tables (SingleCellExperiment's
            // int_colData, reducedDims): listed, its tables walked, no tab.
            const Value* ld = kind == Tab::DFrame ? slot(v, "listData") : v;
            bool plain = false;
            if (ld && ld->type == rser::VECSXP)
                for (const Value* c : ld->items) plain = plain || !nested_column(c);
            if (!plain && ld && ld->length() > 0) {
                row(path, v, "");
                if (depth >= kMaxWalkDepth) return;
                auto nm = str_vec(ld->attr("names"));
                Names rows_only;
                if (names) rows_only.rows = names->rows;
                for (size_t k = 0; k < ld->items.size(); ++k)
                    walk(ld->items[k], child_path(path, k < nm.size() ? nm[k] : "", k), depth + 1,
                         names ? &rows_only : nullptr);
                return;
            }
        }
        if (kind >= 0) {
            Tab t{(Tab::Kind)kind, label_of(path), v, top};
            if (names && (kind == Tab::Frame || kind == Tab::DFrame)) t.row_names = names->rows;
            if (names && (kind == Tab::Sparse || kind == Tab::Matrix || kind == Tab::Dense4)) {
                std::vector<std::string> rn, cn;
                dimnames(kind == Tab::Matrix ? v->attr("dimnames") : slot(v, "Dimnames"), &rn, &cn);
                int64_t nr = 0, nc = 0;
                dims2(kind == Tab::Matrix ? v->attr("dim") : slot(v, "Dim"), &nr, &nc);
                if (rn.empty() && (int64_t)names->rows.size() == nr) t.row_names = names->rows;
                else t.row_names = rn;
                if (cn.empty() && (int64_t)names->cols.size() == nc) t.col_names = names->cols;
                else t.col_names = cn;
            }
            const bool added = add_tab(std::move(t));
            row(path, v, added ? "\xe2\x86\x92 tab " + label_of(path) : "");
            if (kind == Tab::DFrame || kind == Tab::Frame) {
                // nested tables inside a data frame (SingleCellExperiment's
                // reducedDims are matrix columns of a DFrame)
                const Value* ld = kind == Tab::DFrame ? slot(v, "listData") : v;
                if (ld && ld->type == rser::VECSXP && depth < kMaxWalkDepth) {
                    auto nm = str_vec(ld->attr("names"));
                    for (size_t k = 0; k < ld->items.size(); ++k) {
                        const Value* c = ld->items[k];
                        if (nested_column(c)) {
                            // a nested table's rows are the frame's rows
                            Names rows_only;
                            if (names) rows_only.rows = names->rows;
                            walk(c, child_path(path, k < nm.size() ? nm[k] : "", k), depth + 1,
                                 names ? &rows_only : nullptr);
                        }
                    }
                }
            }
            return;
        }
        row(path, v, "");
        if (depth >= kMaxWalkDepth) return;
        switch (v->type) {
            case rser::VECSXP: case rser::EXPRSXP: case rser::LISTSXP: {
                std::vector<std::string> nm = v->type == rser::LISTSXP ? v->tags : str_vec(v->attr("names"));
                for (size_t k = 0; k < v->items.size(); ++k)
                    walk(v->items[k], child_path(path, k < nm.size() ? nm[k] : "", k), depth + 1);
                break;
            }
            case rser::ENVSXP: {
                if (!v->text.empty() || !seen_env.insert(v).second) break;   // a special env, or seen
                for (size_t k = 0; k < v->items.size(); ++k) {
                    const Value* b = v->items[k];
                    if (b->type == rser::CLOSXP || b->type == rser::BUILTINSXP || b->type == rser::SPECIALSXP ||
                        b->type == rser::BCODESXP || b->type == rser::PROMSXP)
                        continue;                       // functions: listed by name only
                    walk(b, child_path(path, k < v->tags.size() ? v->tags[k] : "", k), depth + 1);
                }
                break;
            }
            case rser::S4SXP:
                walk_s4(v, path, depth);
                break;
            default:
                break;
        }
    }

    void walk_s4(const Value* v, const std::string& path, int depth) {
        // A Seurat v5 assay's layers carry no dimnames: rows / columns are the
        // features / cells marked for that layer in the assay's LogMaps.
        std::map<std::string, Names> layer_names;
        if (v->inherits("Assay5")) {
            auto logmap = [](const Value* lm, std::map<std::string, Names>* out, bool rows) {
                int64_t nr, nc;
                if (!lm || lm->type != rser::LGLSXP || !dims2(lm->attr("dim"), &nr, &nc)) return;
                std::vector<std::string> rn, cn;
                dimnames(lm->attr("dimnames"), &rn, &cn);
                if ((int64_t)rn.size() != nr || (int64_t)cn.size() != nc) return;
                for (int64_t c = 0; c < nc; ++c) {
                    std::vector<std::string>& dst = rows ? (*out)[cn[(size_t)c]].rows : (*out)[cn[(size_t)c]].cols;
                    for (int64_t r = 0; r < nr; ++r)
                        if (lm->ints[(size_t)(c * nr + r)] == 1) dst.push_back(rn[(size_t)r]);
                }
            };
            logmap(slot(v, "features"), &layer_names, true);
            logmap(slot(v, "cells"), &layer_names, false);
        }
        // A SummarizedExperiment's assays share its row names (NAMES) and
        // column names (colData row names).
        Names se;
        const bool is_se = slot(v, "assays") && slot(v, "colData") && slot(v, "NAMES");
        if (is_se) {
            se.rows = str_vec(slot(v, "NAMES"));
            if (se.rows.empty()) {              // RangedSummarizedExperiment: names on rowRanges
                const Value* rr = slot(v, "rowRanges");
                se.rows = str_vec(slot(slot(rr, "partitioning"), "NAMES"));
                if (se.rows.empty()) se.rows = str_vec(slot(slot(rr, "ranges"), "NAMES"));
            }
            se.cols = str_vec(slot(slot(v, "colData"), "rownames"));
        }
        // List-like S4 (S4Vectors SimpleList, the assays container): the
        // listData elements stand for the object's own elements.
        const Value* ld = slot(v, "listData");
        for (const auto& [name, sv] : v->attrs) {
            if (name == "class") continue;
            if (sv->type == rser::NILSXP) continue;
            if (name == "listData" && ld && ld->type == rser::VECSXP) {
                auto nm = str_vec(ld->attr("names"));
                for (size_t k = 0; k < ld->items.size(); ++k)
                    walk(ld->items[k], child_path(path, k < nm.size() ? nm[k] : "", k), depth + 1);
                continue;
            }
            const std::string sp = path + "@" + name;
            if (name == "layers" && !layer_names.empty() && sv->type == rser::VECSXP) {
                row(sp, sv, "");
                auto nm = str_vec(sv->attr("names"));
                for (size_t k = 0; k < sv->items.size(); ++k) {
                    const std::string ln = k < nm.size() ? nm[k] : "";
                    auto it = layer_names.find(ln);
                    walk(sv->items[k], child_path(sp, ln, k), depth + 2, it == layer_names.end() ? nullptr : &it->second);
                }
                continue;
            }
            if (is_se && name == "assays") { walk_assays(sv, sp, depth + 1, se); continue; }
            if (is_se && (name == "elementMetadata" || name == "int_elementMetadata")) {
                Names r{se.rows, {}};                 // rowData: one row per feature
                walk(sv, sp, depth + 1, &r);
                continue;
            }
            if (is_se && name == "rowRanges") {
                Names r{se.rows, {}};
                walk(sv, sp, depth + 1, &r);
                continue;
            }
            if (is_se && name == "int_colData") {
                Names r{se.cols, {}};                 // one row per cell (reducedDims, ...)
                walk(sv, sp, depth + 1, &r);
                continue;
            }
            walk(sv, sp, depth + 1);
        }
    }
    // SummarizedExperiment@assays: an Assays object whose data slot is a
    // SimpleList of matrices.
    void walk_assays(const Value* a, const std::string& path, int depth, const Names& se) {
        row(path, a, "");
        const Value* data = slot(a, "data");
        const Value* ld = slot(data, "listData");
        if (!ld || ld->type != rser::VECSXP) { walk_s4(a, path, depth); return; }
        auto nm = str_vec(ld->attr("names"));
        for (size_t k = 0; k < ld->items.size(); ++k)
            walk(ld->items[k], child_path(path, k < nm.size() ? nm[k] : "", k), depth + 1, &se);
    }

    std::shared_ptr<arrow::Table> structure_table() {
        auto col = [](const std::vector<std::string>& v) {
            arrow::StringBuilder b;
            ARROW_UNUSED(b.AppendValues(v));
            std::shared_ptr<arrow::Array> a;
            ARROW_UNUSED(b.Finish(&a));
            return a;
        };
        return arrow::Table::Make(arrow::schema({arrow::field("path", arrow::utf8()), arrow::field("class", arrow::utf8()),
                                                 arrow::field("type", arrow::utf8()), arrow::field("size", arrow::utf8()),
                                                 arrow::field("value", arrow::utf8())}),
                                  {col(spath), col(sclass), col(stype), col(ssize), col(sprev)});
    }
};

// Tabs for one top-level object.
void add_object(State& st, const std::string& name, const Value* v) {
    if (v->type == rser::NILSXP) return;
    const std::string base = st.rds ? "" : name;
    const int kind = Walker::tab_kind(v, true);
    if (kind >= 0) {
        std::string label = name;
        if (label.empty()) {
            const size_t slash = st.path.find_last_of('/');
            label = slash == std::string::npos ? st.path : st.path.substr(slash + 1);
        }
        if (st.tabs.size() < kMaxTabs) st.tabs.push_back({(Tab::Kind)kind, label, v, name});
        else ++st.skipped_tabs;
        // a data frame's nested tables (rare at top level) are not split out
        return;
    }
    // A list / S4 object / environment: its structure, then its table-like parts.
    const size_t at = st.tabs.size();
    if (st.tabs.size() < kMaxTabs) {
        Tab s{Tab::Structure, st.rds ? "structure" : name + " (structure)", v, name};
        st.tabs.push_back(s);
    }
    Walker w{st, base};
    w.walk(v, "", 0);
    if (at < st.tabs.size() && st.tabs[at].kind == Tab::Structure) {
        st.tabs[at].structure = w.structure_table();
        st.tabs[at].what = class_label(v);
    }
}

class RdataSource : public WorkbookSource {
public:
    RdataSource(std::shared_ptr<State> st, size_t idx)
        : WorkbookSource(nullptr, st->path, ""), st_(std::move(st)), idx_(idx) {}

    std::string tab_label() const override { return st_->tabs[idx_].label; }
    std::shared_ptr<arrow::Schema> schema() const override { ensure(); return MemoryTableSource::schema(); }
    int64_t total_rows() const override { ensure(); return MemoryTableSource::total_rows(); }
    ChunkMeta chunk_meta(int i) const override { ensure(); return MemoryTableSource::chunk_meta(i); }
    arrow::Status read_chunk(int i, const std::vector<int>& cols, std::shared_ptr<arrow::Table>* out) override {
        ensure();
        return MemoryTableSource::read_chunk(i, cols, out);
    }
    std::string footer() const override { ensure(); return MemoryTableSource::footer(); }
    arrow::Status read_status() const override { ensure(); return status_; }
    PreviewLimit preview_limit() const override { ensure(); return limit_; }
    bool show_cells_in_full() const override { return st_->tabs[idx_].kind == Tab::Structure; }
    std::vector<std::unique_ptr<TabularSource>> open_sibling_sheets() const override;
    // An export of a capped matrix preview streams the whole matrix.
    std::unique_ptr<TabularSource> full_matrix() const override;

private:
    std::shared_ptr<State> st_;
    size_t idx_;
    mutable bool built_ = false;
    mutable arrow::Status status_;
    mutable PreviewLimit limit_;

    void ensure() const {
        if (built_) return;
        built_ = true;
        auto* self = const_cast<RdataSource*>(this);
        const Tab& t = st_->tabs[idx_];
        std::string kind;
        auto r = build(*st_, t, &kind, &self->limit_);
        std::shared_ptr<arrow::Table> tbl;
        if (r.ok() && *r) {
            tbl = *r;
        } else {
            const std::string msg = r.ok() ? "empty" : r.status().message();
            arrow::StringBuilder b;
            ARROW_UNUSED(b.Append(msg));
            std::shared_ptr<arrow::Array> a;
            ARROW_UNUSED(b.Finish(&a));
            tbl = arrow::Table::Make(arrow::schema({arrow::field("error", arrow::utf8())}), {a});
            kind = "error: " + msg;
            self->status_ = arrow::Status::IOError(t.label + ": " + msg);
        }
        std::string footer = std::string("Format: R ") + (st_->rds ? ".rds" : ".RData") + "  |  " + kind;
        if (!st_->rds && t.kind != Tab::Structure && !t.object.empty() && t.label != t.object)
            footer += "  |  object: " + t.object;
        else if (!st_->rds && t.label == t.object)
            footer += "  |  object: " + t.object;
        if (idx_ == 0 && st_->tabs.size() > 1)
            footer += "  |  +" + std::to_string(st_->tabs.size() - 1) + " more tab(s)";
        if (idx_ == 0 && st_->skipped_tabs)
            footer += "  |  " + std::to_string(st_->skipped_tabs) + " more table-like parts not given tabs";
        self->replace_table(std::move(tbl), std::move(footer));
    }
};

// --matrix long: every stored value of a sparse matrix as (row, col, value),
// streamed in chunks of stored entries.
class RSparseLongSource : public TabularSource {
public:
    RSparseLongSource(std::shared_ptr<State> st, size_t idx) : st_(std::move(st)), idx_(idx) {
        const Tab& t = st_->tabs[idx_];
        ok_ = Sparse::make(t.v, &s_);
        rn_ = t.row_names;
        cn_ = t.col_names;
        if (rn_.empty() && cn_.empty()) dimnames(slot(t.v, "Dimnames"), &rn_, &cn_);
        if ((int64_t)rn_.size() != s_.nr) rn_.clear();
        if ((int64_t)cn_.size() != s_.nc) cn_.clear();
        auto name_t = [](bool named) { return named ? arrow::dictionary(arrow::int32(), arrow::utf8()) : arrow::int64(); };
        schema_ = arrow::schema({arrow::field("row", name_t(!rn_.empty())), arrow::field("col", name_t(!cn_.empty())),
                                 arrow::field("value", s_.logical ? arrow::boolean() : arrow::float64())});
        if (!ok_) return;
        // Entries in storage order, collected once as (row, col, k) — the file
        // is untrusted, so out-of-range entries are dropped here.
        s_.each([&](int64_t r, int64_t c, int64_t k) {
            ent_.push_back({(int32_t)r, (int32_t)c, k});
            return true;
        });
        auto dict = [](const std::vector<std::string>& v) {
            arrow::StringBuilder b;
            ARROW_UNUSED(b.AppendValues(v));
            std::shared_ptr<arrow::Array> a;
            ARROW_UNUSED(b.Finish(&a));
            return a;
        };
        if (!rn_.empty()) rdict_ = dict(rn_);
        if (!cn_.empty()) cdict_ = dict(cn_);
    }
    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return (int64_t)ent_.size(); }
    int num_chunks() const override { return (int)(((int64_t)ent_.size() + kLongChunk - 1) / kLongChunk); }
    ChunkMeta chunk_meta(int i) const override {
        const int64_t first = (int64_t)i * kLongChunk;
        return {first, std::max<int64_t>(0, std::min<int64_t>(kLongChunk, (int64_t)ent_.size() - first))};
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols, std::shared_ptr<arrow::Table>* out) override {
        const ChunkMeta m = chunk_meta(i);
        auto axis = [&](bool rows, const std::shared_ptr<arrow::Array>& dict) -> arrow::Result<std::shared_ptr<arrow::Array>> {
            if (dict) {
                arrow::Int32Builder b;
                for (int64_t k = m.first_row; k < m.first_row + m.num_rows; ++k)
                    ARROW_RETURN_NOT_OK(b.Append(rows ? ent_[(size_t)k].r : ent_[(size_t)k].c));
                std::shared_ptr<arrow::Array> idx;
                ARROW_RETURN_NOT_OK(b.Finish(&idx));
                return arrow::DictionaryArray::FromArrays(arrow::dictionary(arrow::int32(), arrow::utf8()), idx, dict);
            }
            arrow::Int64Builder b;                          // 1-based, as R indexes
            for (int64_t k = m.first_row; k < m.first_row + m.num_rows; ++k)
                ARROW_RETURN_NOT_OK(b.Append(1 + (rows ? ent_[(size_t)k].r : ent_[(size_t)k].c)));
            std::shared_ptr<arrow::Array> a;
            ARROW_RETURN_NOT_OK(b.Finish(&a));
            return a;
        };
        arrow::FieldVector f;
        arrow::ArrayVector a;
        for (int c : cols) {
            if (c == 0) { ARROW_ASSIGN_OR_RAISE(auto x, axis(true, rdict_)); a.push_back(x); }
            else if (c == 1) { ARROW_ASSIGN_OR_RAISE(auto x, axis(false, cdict_)); a.push_back(x); }
            else if (c == 2) {
                std::shared_ptr<arrow::Array> x;
                if (s_.logical) {
                    arrow::BooleanBuilder b;
                    for (int64_t k = m.first_row; k < m.first_row + m.num_rows; ++k) {
                        const double d = s_.value(ent_[(size_t)k].k);
                        ARROW_RETURN_NOT_OK(std::isnan(d) ? b.AppendNull() : b.Append(d != 0));
                    }
                    ARROW_RETURN_NOT_OK(b.Finish(&x));
                } else {
                    arrow::DoubleBuilder b;
                    for (int64_t k = m.first_row; k < m.first_row + m.num_rows; ++k) {
                        const double d = s_.value(ent_[(size_t)k].k);
                        ARROW_RETURN_NOT_OK(std::isnan(d) ? b.AppendNull() : b.Append(d));
                    }
                    ARROW_RETURN_NOT_OK(b.Finish(&x));
                }
                a.push_back(x);
            } else {
                continue;
            }
            f.push_back(schema_->field(c));
        }
        *out = arrow::Table::Make(arrow::schema(f), a, m.num_rows);
        return arrow::Status::OK();
    }
    const std::string& path() const override { return st_->path; }
    std::string tab_label() const override { return st_->tabs[idx_].label; }
    std::string footer() const override {
        return std::string("Format: R ") + (st_->rds ? ".rds" : ".RData") + "  |  " + s_.cls + " " +
               shape(s_.nr, s_.nc) + ", one row per stored value";
    }
    arrow::Status read_status() const override {
        return ok_ ? arrow::Status::OK() : arrow::Status::IOError("not a sparse matrix");
    }
    std::vector<std::unique_ptr<TabularSource>> expand_tabs() const override;

private:
    struct Ent { int32_t r, c; int64_t k; };
    std::shared_ptr<State> st_;
    size_t idx_;
    Sparse s_;
    bool ok_ = false;
    std::vector<std::string> rn_, cn_;
    std::shared_ptr<arrow::Array> rdict_, cdict_;
    std::vector<Ent> ent_;
    std::shared_ptr<arrow::Schema> schema_;
};

// A whole matrix (sparse, or dense past kMaxMatrixCols) streamed in blocks of
// rows across every column, for exports of a capped preview.
class RWideSource : public TabularSource {
public:
    RWideSource(std::shared_ptr<State> st, size_t idx) : st_(std::move(st)), idx_(idx) {
        const Tab& t = st_->tabs[idx_];
        rn_ = t.row_names;
        cn_ = t.col_names;
        sparse_ = t.kind == Tab::Sparse;
        if (sparse_) {
            ok_ = Sparse::make(t.v, &s_);
            nr_ = s_.nr; nc_ = s_.nc;
            if (rn_.empty() && cn_.empty()) dimnames(slot(t.v, "Dimnames"), &rn_, &cn_);
            if (ok_) {
                // CSR index over the stored entries: row → (col, k), in row order.
                row_ptr_.assign((size_t)nr_ + 1, 0);
                s_.each([&](int64_t r, int64_t, int64_t) { ++row_ptr_[(size_t)r + 1]; return true; });
                for (size_t r = 1; r < row_ptr_.size(); ++r) row_ptr_[r] += row_ptr_[r - 1];
                ent_.resize((size_t)row_ptr_.back());
                std::vector<int64_t> fill(row_ptr_.begin(), row_ptr_.end() - 1);
                s_.each([&](int64_t r, int64_t c, int64_t k) {
                    ent_[(size_t)fill[(size_t)r]++] = {(int32_t)c, k};
                    return true;
                });
            }
        } else {
            x_ = t.kind == Tab::Matrix ? t.v : slot(t.v, "x");
            ok_ = dims2(t.kind == Tab::Matrix ? t.v->attr("dim") : slot(t.v, "Dim"), &nr_, &nc_) && x_;
            if (rn_.empty() && cn_.empty())
                dimnames(t.kind == Tab::Matrix ? t.v->attr("dimnames") : slot(t.v, "Dimnames"), &rn_, &cn_);
        }
        if ((int64_t)rn_.size() != nr_) rn_.clear();
        if ((int64_t)cn_.size() != nc_) cn_.clear();
        arrow::FieldVector f;
        if (!rn_.empty()) f.push_back(arrow::field("index", arrow::utf8()));
        std::shared_ptr<arrow::DataType> vt = arrow::float64();
        if (sparse_ && s_.logical) vt = arrow::boolean();
        if (!sparse_ && ok_ && nr_ > 0) {
            auto a = to_array(x_, 0, 1);
            if (a.ok()) vt = (*a)->type();
        }
        for (int64_t c = 0; c < nc_; ++c)
            f.push_back(arrow::field(cn_.empty() || cn_[(size_t)c].empty() ? "V" + std::to_string(c + 1) : cn_[(size_t)c], vt));
        schema_ = arrow::schema(f);
        rows_per_ = std::max<int64_t>(1, std::min<int64_t>(4096, (int64_t(4) << 20) / std::max<int64_t>(1, nc_)));
    }
    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return ok_ ? nr_ : 0; }
    int num_chunks() const override { return ok_ ? (int)((nr_ + rows_per_ - 1) / rows_per_) : 0; }
    ChunkMeta chunk_meta(int i) const override {
        const int64_t first = (int64_t)i * rows_per_;
        return {first, std::max<int64_t>(0, std::min(rows_per_, nr_ - first))};
    }
    arrow::Status read_status() const override {
        return ok_ ? arrow::Status::OK() : arrow::Status::IOError("unreadable matrix");
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols, std::shared_ptr<arrow::Table>* out) override {
        const ChunkMeta m = chunk_meta(i);
        const int off = rn_.empty() ? 0 : 1;
        std::vector<double> block;
        if (sparse_) {                                      // dense block of the chunk's rows
            block.assign((size_t)(m.num_rows * nc_), 0.0);
            for (int64_t r = 0; r < m.num_rows; ++r) {
                const size_t row = (size_t)(m.first_row + r);
                for (int64_t e = row_ptr_[row]; e < row_ptr_[row + 1]; ++e)
                    block[(size_t)(ent_[(size_t)e].c * m.num_rows + r)] += s_.value(ent_[(size_t)e].k);
            }
        }
        arrow::FieldVector f;
        arrow::ArrayVector a;
        for (int c : cols) {
            if (c < 0 || c >= schema_->num_fields()) continue;
            std::shared_ptr<arrow::Array> arr;
            if (c < off) {
                ARROW_ASSIGN_OR_RAISE(arr, strings_array(std::vector<std::string>(rn_.begin() + m.first_row,
                                                                                 rn_.begin() + m.first_row + m.num_rows),
                                                         (size_t)m.num_rows));
            } else if (!sparse_) {
                ARROW_ASSIGN_OR_RAISE(arr, to_array(x_, (int64_t)(c - off) * nr_ + m.first_row, m.num_rows));
            } else {
                const double* col = block.data() + (size_t)(c - off) * (size_t)m.num_rows;
                if (s_.logical) {
                    arrow::BooleanBuilder b;
                    for (int64_t r = 0; r < m.num_rows; ++r)
                        ARROW_RETURN_NOT_OK(std::isnan(col[r]) ? b.AppendNull() : b.Append(col[r] != 0));
                    ARROW_RETURN_NOT_OK(b.Finish(&arr));
                } else {
                    arrow::DoubleBuilder b;
                    for (int64_t r = 0; r < m.num_rows; ++r)
                        ARROW_RETURN_NOT_OK(std::isnan(col[r]) ? b.AppendNull() : b.Append(col[r]));
                    ARROW_RETURN_NOT_OK(b.Finish(&arr));
                }
            }
            f.push_back(schema_->field(c));
            a.push_back(arr);
        }
        *out = arrow::Table::Make(arrow::schema(f), a, m.num_rows);
        return arrow::Status::OK();
    }
    const std::string& path() const override { return st_->path; }
    std::string tab_label() const override { return st_->tabs[idx_].label; }
    std::string footer() const override {
        return std::string("Format: R ") + (st_->rds ? ".rds" : ".RData") + "  |  matrix " + shape(nr_, nc_) +
               " (all rows and columns)";
    }

private:
    struct Ent { int32_t c; int64_t k; };
    std::shared_ptr<State> st_;
    size_t idx_;
    bool sparse_ = false, ok_ = false;
    Sparse s_;
    const Value* x_ = nullptr;
    int64_t nr_ = 0, nc_ = 0, rows_per_ = 1;
    std::vector<std::string> rn_, cn_;
    std::vector<int64_t> row_ptr_;
    std::vector<Ent> ent_;
    std::shared_ptr<arrow::Schema> schema_;
};

std::unique_ptr<TabularSource> RdataSource::full_matrix() const {
    const Tab::Kind k = st_->tabs[idx_].kind;
    if (k != Tab::Sparse && k != Tab::Matrix && k != Tab::Dense4) return nullptr;
    return std::make_unique<RWideSource>(st_, idx_);
}

std::unique_ptr<TabularSource> make_tab(const std::shared_ptr<State>& st, size_t k) {
    if (st->matrix_long && st->tabs[k].kind == Tab::Sparse) return std::make_unique<RSparseLongSource>(st, k);
    return std::make_unique<RdataSource>(st, k);
}
std::vector<std::unique_ptr<TabularSource>> siblings(const std::shared_ptr<State>& st, size_t idx) {
    std::vector<std::unique_ptr<TabularSource>> v;
    if (idx != 0) return v;
    for (size_t k = 1; k < st->tabs.size(); ++k) v.push_back(make_tab(st, k));
    return v;
}
std::vector<std::unique_ptr<TabularSource>> RdataSource::open_sibling_sheets() const { return siblings(st_, idx_); }
std::vector<std::unique_ptr<TabularSource>> RSparseLongSource::expand_tabs() const { return siblings(st_, idx_); }

std::string open_from_stream(std::shared_ptr<arrow::io::InputStream> in, const std::string& path, bool rdata_ext,
                             bool matrix_long, std::shared_ptr<State>* out) {
    auto st = std::make_shared<State>();
    st->path = path;
    st->matrix_long = matrix_long;
    if (auto e = rser::read(std::move(in), &st->file); !e.empty()) return e;
    st->rds = !st->file.rdata && !rdata_ext;
    try {
        for (const auto& [name, v] : st->file.objects) add_object(*st, name, v);
    } catch (const std::bad_alloc&) {
        return "out of memory";
    }
    if (st->tabs.empty()) return "no objects";
    *out = std::move(st);
    return "";
}

}  // namespace

std::string open_rdata_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    std::shared_ptr<arrow::io::InputStream> in;
    if (auto e = open_decoded_file(path, &in); !e.empty()) return "'" + path + "': " + e;
    const std::string bare = strip_compression_suffix(path);
    std::shared_ptr<State> st;
    if (auto e = open_from_stream(std::move(in), path, fends_ci(bare, ".rdata") || fends_ci(bare, ".rda"),
                                  cfg.matrix == "long", &st);
        !e.empty())
        return "'" + path + "': " + e;
    *out = make_tab(st, 0);
    return "";
}

#ifdef VV_FUZZ
// Parse an untrusted buffer as an R file (any compression) and build every
// tab's table; abort when a built table is not valid.
void rdata_fuzz_one(const uint8_t* buf, size_t n) {
    auto in = std::make_shared<arrow::io::BufferReader>(std::make_shared<arrow::Buffer>(buf, (int64_t)n));
    std::shared_ptr<arrow::io::InputStream> dec;
    if (!decode_stream(sniff_codec(buf, n), in, &dec).empty()) return;
    for (bool lng : {false, true}) {
        std::shared_ptr<State> st;
        auto in2 = std::make_shared<arrow::io::BufferReader>(std::make_shared<arrow::Buffer>(buf, (int64_t)n));
        if (!decode_stream(sniff_codec(buf, n), in2, &dec).empty()) return;
        if (!open_from_stream(dec, "fuzz", false, lng, &st).empty()) return;
        for (size_t k = 0; k < st->tabs.size(); ++k) {
            auto src = make_tab(st, k);
            for (int c = 0; c < std::min(src->num_chunks(), 3); ++c) {
                std::vector<int> cols;
                for (int j = 0; j < src->schema()->num_fields(); ++j) cols.push_back(j);
                std::shared_ptr<arrow::Table> t;
                if (!src->read_chunk(c, cols, &t).ok() || !t) continue;
                if (auto s = t->ValidateFull(); !s.ok()) {
                    std::fprintf(stderr, "invalid: %s\n", s.ToString().c_str());
                    std::abort();
                }
            }
        }
    }
}
#endif
