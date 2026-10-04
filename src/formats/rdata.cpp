// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// R data files: .rds (one object) and .RData / .rda (several, one tab each),
// read with the vendored librdata. Data frames, atomic vectors and dense
// matrices become tables: factors as categorical columns, Date / POSIXct as
// date32 / timestamp, character row names as an `index` column. Lists and S4
// objects (Seurat, SingleCellExperiment, sparse Matrix) are not supported.

#include "internal.hpp"

extern "C" {
#include <rdata.h>
}

namespace {

// One column as librdata hands it over (data before names).
struct RCol {
    std::string                             name;      // a vector's own name (.RData key)
    rdata_type_t                            type = RDATA_TYPE_STRING;
    std::vector<double>                     d;          // REAL, DATE, TIMESTAMP
    std::vector<int32_t>                    i;          // INT32, LOGICAL, factor codes
    std::vector<std::optional<std::string>> s;          // STRING
    std::vector<std::string>                levels;     // a factor's levels
    long                                    count = 0;
};

// One R object being read.
struct RObj {
    std::string              name;
    std::vector<RCol>        cols;
    std::vector<std::string> col_names, row_names;
    std::vector<std::optional<std::vector<std::string>>> dim_names;  // per axis; nullopt: unnamed
    std::vector<int32_t>     dims;
};

struct Ctx {
    std::vector<RObj>        objs;
    std::vector<std::string> pending_levels;   // a factor's levels come before its codes
    std::string              error;
    RObj& cur() { if (objs.empty()) objs.emplace_back(); return objs.back(); }
};

bool is_na_real(double v) {
    if (!std::isnan(v)) return false;
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    return (bits & 0xffffffffu) == 1954;          // R's NA_real_ (NaN stays NaN)
}

int on_table(const char* name, void* ctx) {
    auto* c = static_cast<Ctx*>(ctx);
    c->objs.emplace_back();
    c->objs.back().name = name ? valid_utf8(name) : "";
    c->pending_levels.clear();
    return 0;
}
int on_column(const char* name, rdata_type_t type, void* data, long count, void* ctx) {
    auto* c = static_cast<Ctx*>(ctx);
    RCol col;
    col.name = name ? valid_utf8(name) : "";
    col.type = type;
    col.count = count;
    switch (type) {
        case RDATA_TYPE_REAL: case RDATA_TYPE_DATE: case RDATA_TYPE_TIMESTAMP:
            if (!data) col.count = 0;
            else col.d.assign(static_cast<double*>(data), static_cast<double*>(data) + count);
            break;
        case RDATA_TYPE_INT32: case RDATA_TYPE_LOGICAL:
            if (!data) col.count = 0;
            else col.i.assign(static_cast<int32_t*>(data), static_cast<int32_t*>(data) + count);
            if (type == RDATA_TYPE_INT32) col.levels = std::move(c->pending_levels);
            break;
        case RDATA_TYPE_STRING:                    // values follow (text handler)
            break;
    }
    c->pending_levels.clear();
    c->cur().cols.push_back(std::move(col));
    return 0;
}
int on_text(const char* value, int, void* ctx) {
    auto* c = static_cast<Ctx*>(ctx);
    RObj& o = c->cur();
    if (o.cols.empty() || o.cols.back().type != RDATA_TYPE_STRING) return 0;
    o.cols.back().s.push_back(value ? std::optional<std::string>(valid_utf8(value)) : std::nullopt);
    return 0;
}
int on_level(const char* value, int, void* ctx) {
    static_cast<Ctx*>(ctx)->pending_levels.push_back(value ? valid_utf8(value) : "NA");
    return 0;
}
int on_col_name(const char* value, int, void* ctx) {
    static_cast<Ctx*>(ctx)->cur().col_names.push_back(value ? valid_utf8(value) : "");
    return 0;
}
int on_row_name(const char* value, int, void* ctx) {
    static_cast<Ctx*>(ctx)->cur().row_names.push_back(value ? valid_utf8(value) : "");
    return 0;
}
int on_dim(const char*, rdata_type_t, void* data, long count, void* ctx) {
    const auto* v = static_cast<int32_t*>(data);
    if (v) static_cast<Ctx*>(ctx)->cur().dims.assign(v, v + count);
    return 0;
}
int on_dim_name(const char* value, int index, void* ctx) {
    auto& axes = static_cast<Ctx*>(ctx)->cur().dim_names;
    if (index == 0 || axes.empty()) axes.emplace_back(std::vector<std::string>{});
    if (!value) axes.back().reset();              // NULL: this axis has no names
    else if (axes.back()) axes.back()->push_back(valid_utf8(value));
    return 0;
}
void on_error(const char* msg, void* ctx) {
    auto* c = static_cast<Ctx*>(ctx);
    if (c->error.empty() && msg) c->error = msg;
}

// The handlers run inside librdata's C frames: an exception (bad_alloc on a
// huge declared length) must not unwind through them, so it aborts the parse.
template <auto F> struct Guard;
template <typename... A, int (*F)(A...)> struct Guard<F> {
    static int call(A... a) noexcept {
        try { return F(a...); } catch (...) { return 1; }
    }
};

// One column's values [first, first + n) as an Arrow array.
arrow::Result<std::shared_ptr<arrow::Array>> to_array(const RCol& col, int64_t first, int64_t n) {
    std::shared_ptr<arrow::Array> out;
    switch (col.type) {
        case RDATA_TYPE_STRING: {
            arrow::StringBuilder b;
            for (int64_t k = first; k < first + n; ++k) {
                const auto& v = (size_t)k < col.s.size() ? col.s[(size_t)k] : std::nullopt;
                ARROW_RETURN_NOT_OK(v ? b.Append(*v) : b.AppendNull());
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        case RDATA_TYPE_REAL: {
            arrow::DoubleBuilder b;
            for (int64_t k = first; k < first + n; ++k) {
                const double v = col.d[(size_t)k];
                ARROW_RETURN_NOT_OK(is_na_real(v) ? b.AppendNull() : b.Append(v));
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        case RDATA_TYPE_LOGICAL: {
            arrow::BooleanBuilder b;
            for (int64_t k = first; k < first + n; ++k) {
                const int32_t v = col.i[(size_t)k];
                ARROW_RETURN_NOT_OK(v == INT32_MIN ? b.AppendNull() : b.Append(v != 0));
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        case RDATA_TYPE_INT32: {
            arrow::Int32Builder b;
            for (int64_t k = first; k < first + n; ++k) {
                const int32_t v = col.i[(size_t)k];
                // factor codes are 1-based; out-of-range codes become null
                if (v == INT32_MIN || (!col.levels.empty() && (v < 1 || v > (int32_t)col.levels.size())))
                    ARROW_RETURN_NOT_OK(b.AppendNull());
                else
                    ARROW_RETURN_NOT_OK(b.Append(col.levels.empty() ? v : v - 1));
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            if (col.levels.empty()) return out;
            arrow::StringBuilder lb;
            ARROW_RETURN_NOT_OK(lb.AppendValues(col.levels));
            std::shared_ptr<arrow::Array> dict;
            ARROW_RETURN_NOT_OK(lb.Finish(&dict));
            return arrow::DictionaryArray::FromArrays(arrow::dictionary(arrow::int32(), arrow::utf8()),
                                                      out, dict);
        }
        case RDATA_TYPE_DATE: {
            arrow::Date32Builder b;
            for (int64_t k = first; k < first + n; ++k) {
                const double v = col.d[(size_t)k];
                // days since 1970; outside the int32 range (or NaN / NA) is null
                ARROW_RETURN_NOT_OK(std::fabs(v) < 2147483647.0 ? b.Append((int32_t)std::floor(v))
                                                                 : b.AppendNull());
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
        case RDATA_TYPE_TIMESTAMP: {
            arrow::TimestampBuilder b(arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"),
                                      arrow::default_memory_pool());
            for (int64_t k = first; k < first + n; ++k) {
                const double v = col.d[(size_t)k];
                // seconds since 1970 as microseconds; beyond int64 (or NaN / NA) is null
                ARROW_RETURN_NOT_OK(std::fabs(v) < 9.2e12 ? b.Append((int64_t)std::llround(v * 1e6))
                                                          : b.AppendNull());
            }
            ARROW_RETURN_NOT_OK(b.Finish(&out));
            return out;
        }
    }
    return arrow::Status::NotImplemented("R column type");
}

std::shared_ptr<arrow::Array> strings(const std::vector<std::string>& v, size_t first, size_t n) {
    arrow::StringBuilder b;
    for (size_t k = first; k < first + n && k < v.size(); ++k) (void)b.Append(v[k]);
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    return a;
}

// An object as a table (and what kind of object it was).
arrow::Result<std::shared_ptr<arrow::Table>> to_table(const RObj& o, std::string* kind) {
    arrow::FieldVector fields;
    arrow::ArrayVector arrays;
    if (o.dims.size() == 2 && o.cols.size() == 1 && o.dims[0] >= 0 && o.dims[1] >= 0 &&
        o.cols[0].count == (int64_t)o.dims[0] * o.dims[1]) {
        // A matrix: column-major values; dimnames rows first, then columns.
        const int64_t nr = o.dims[0], nc = o.dims[1];
        std::vector<std::string> rn, cn;
        if (o.dim_names.size() == 2) {
            if (o.dim_names[0] && (int64_t)o.dim_names[0]->size() == nr) rn = *o.dim_names[0];
            if (o.dim_names[1] && (int64_t)o.dim_names[1]->size() == nc) cn = *o.dim_names[1];
        }
        if (!rn.empty()) {
            fields.push_back(arrow::field("index", arrow::utf8()));
            arrays.push_back(strings(rn, 0, (size_t)nr));
        }
        for (int64_t c = 0; c < nc; ++c) {
            ARROW_ASSIGN_OR_RAISE(auto a, to_array(o.cols[0], c * nr, nr));
            fields.push_back(arrow::field(cn.empty() || cn[(size_t)c].empty()
                                              ? "V" + std::to_string(c + 1) : cn[(size_t)c],
                                          a->type()));
            arrays.push_back(a);
        }
        *kind = "matrix " + std::to_string(nr) + " \xc3\x97 " + std::to_string(nc);
        return arrow::Table::Make(arrow::schema(fields), arrays, nr);
    }
    int64_t rows = 0;
    for (const auto& c : o.cols) rows = std::max<int64_t>(rows, c.count);
    for (const auto& c : o.cols)
        if (c.count != rows || (c.type == RDATA_TYPE_STRING && (int64_t)c.s.size() != c.count))
            return arrow::Status::Invalid("columns of different lengths");
    if (!o.row_names.empty() && (int64_t)o.row_names.size() == rows) {
        fields.push_back(arrow::field("index", arrow::utf8()));
        arrays.push_back(strings(o.row_names, 0, (size_t)rows));
    }
    for (size_t k = 0; k < o.cols.size(); ++k) {
        ARROW_ASSIGN_OR_RAISE(auto a, to_array(o.cols[k], 0, rows));
        std::string nm = k < o.col_names.size() && !o.col_names[k].empty() ? o.col_names[k]
                       : !o.cols[k].name.empty() ? o.cols[k].name
                       : o.cols.size() == 1 ? "value" : "V" + std::to_string(k + 1);
        fields.push_back(arrow::field(nm, a->type()));
        arrays.push_back(a);
    }
    *kind = o.col_names.empty() && o.cols.size() == 1 ? "vector" : "data frame";
    return arrow::Table::Make(arrow::schema(fields), arrays, rows);
}

struct RTable { std::string name, kind; std::shared_ptr<arrow::Table> table; };

class RdataSource : public WorkbookSource {
public:
    RdataSource(std::shared_ptr<std::vector<RTable>> all, size_t idx, std::string path, bool rds)
        : WorkbookSource((*all)[idx].table, std::move(path), ""), all_(std::move(all)), idx_(idx),
          rds_(rds) {
        const RTable& t = (*all_)[idx_];
        footer_str_ = std::string("Format: R ") + (rds_ ? ".rds" : ".RData") + "  |  " + t.kind +
                      (rds_ ? "" : "  |  object: " + t.name);
        if (idx_ == 0 && all_->size() > 1)
            footer_str_ += "  |  +" + std::to_string(all_->size() - 1) + " more object(s)";
    }
    std::string tab_label() const override {
        const std::string& n = (*all_)[idx_].name;
        if (!n.empty()) return n;
        const size_t slash = path().find_last_of('/');
        return slash == std::string::npos ? path() : path().substr(slash + 1);
    }
    std::vector<std::unique_ptr<TabularSource>> open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> v;
        if (idx_ != 0) return v;
        for (size_t k = 1; k < all_->size(); ++k)
            v.push_back(std::make_unique<RdataSource>(all_, k, path(), rds_));
        return v;
    }
private:
    std::shared_ptr<std::vector<RTable>> all_;
    size_t idx_;
    bool   rds_;
};

}  // namespace

namespace {
void set_handlers(rdata_parser_t* p) {
    rdata_set_table_handler(p, Guard<on_table>::call);
    rdata_set_column_handler(p, Guard<on_column>::call);
    rdata_set_column_name_handler(p, Guard<on_col_name>::call);
    rdata_set_row_name_handler(p, Guard<on_row_name>::call);
    rdata_set_text_value_handler(p, Guard<on_text>::call);
    rdata_set_value_label_handler(p, Guard<on_level>::call);
    rdata_set_dim_handler(p, Guard<on_dim>::call);
    rdata_set_dim_name_handler(p, Guard<on_dim_name>::call);
    rdata_set_error_handler(p, on_error);
}
}  // namespace

std::string open_rdata_source(const std::string& path, std::unique_ptr<TabularSource>* out) {
    Ctx ctx;
    rdata_parser_t* p = rdata_parser_init();
    if (!p) return "out of memory";
    set_handlers(p);
    const rdata_error_t e = rdata_parse(p, path.c_str(), &ctx);
    rdata_parser_free(p);
    if (e != RDATA_OK) {
        std::string why = rdata_error_message(e);
        if (e == RDATA_ERROR_UNSUPPORTED_S_EXPRESSION || e == RDATA_ERROR_UNSUPPORTED_STORAGE_CLASS)
            why += " (vv reads data frames, vectors and dense matrices; lists and S4 objects "
                   "such as Seurat or sparse matrices are not supported)";
        if (!ctx.error.empty()) why += ": " + ctx.error;
        return "'" + path + "': " + why;
    }
    auto all = std::make_shared<std::vector<RTable>>();
    std::vector<std::string> skipped;
    for (const RObj& o : ctx.objs) {
        if (o.cols.empty()) { if (!o.name.empty()) skipped.push_back(o.name); continue; }
        std::string kind;
        auto t = to_table(o, &kind);
        if (!t.ok()) { skipped.push_back(o.name.empty() ? "object" : o.name); continue; }
        all->push_back({o.name, kind, *t});
    }
    if (all->empty())
        return "'" + path + "': no data frame, vector or matrix found (vv does not read R lists "
               "or S4 objects such as Seurat, SingleCellExperiment or sparse matrices)";
    const bool rds = !fends_ci(strip_compression_suffix(path), ".rdata") &&
                     !fends_ci(strip_compression_suffix(path), ".rda");
    auto src = std::make_unique<RdataSource>(all, 0, path, rds);
    *out = std::move(src);
    return "";
}

#ifdef VV_FUZZ
// Parse an untrusted buffer as an R file (librdata over in-memory I/O) and
// build every object's table; abort when a built table is not valid.
namespace {
struct MemIO { const uint8_t* p; size_t n; size_t pos; };
int mem_open(const char*, void*) { return 0; }
int mem_close(void*) { return 0; }
rdata_off_t mem_seek(rdata_off_t off, rdata_io_flags_t whence, void* io) {
    auto* m = static_cast<MemIO*>(io);
    int64_t base = whence == RDATA_SEEK_SET ? 0 : whence == RDATA_SEEK_CUR ? (int64_t)m->pos : (int64_t)m->n;
    const int64_t to = base + (int64_t)off;
    if (to < 0 || to > (int64_t)m->n) return -1;
    m->pos = (size_t)to;
    return (rdata_off_t)to;
}
ssize_t mem_read(void* buf, size_t nbyte, void* io) {
    auto* m = static_cast<MemIO*>(io);
    const size_t k = std::min(nbyte, m->n - m->pos);
    std::memcpy(buf, m->p + m->pos, k);
    m->pos += k;
    return (ssize_t)k;
}
rdata_error_t mem_update(long, rdata_progress_handler, void*, void*) { return RDATA_OK; }
}  // namespace

void rdata_fuzz_one(const uint8_t* buf, size_t n) {
    MemIO io{buf, n, 0};
    Ctx ctx;
    rdata_parser_t* p = rdata_parser_init();
    set_handlers(p);
    rdata_set_open_handler(p, mem_open);
    rdata_set_close_handler(p, mem_close);
    rdata_set_seek_handler(p, mem_seek);
    rdata_set_read_handler(p, mem_read);
    rdata_set_update_handler(p, mem_update);
    rdata_set_io_ctx(p, &io);
    const rdata_error_t e = rdata_parse(p, "fuzz", &ctx);
    rdata_parser_free(p);
    if (e != RDATA_OK) return;                    // as open_rdata_source
    for (const RObj& o : ctx.objs) {
        std::string kind;
        auto t = to_table(o, &kind);
        if (t.ok()) { auto st = (*t)->ValidateFull(); if (!st.ok()) { std::fprintf(stderr, "invalid: %s\n", st.ToString().c_str()); std::abort(); } }
    }
}
#endif
