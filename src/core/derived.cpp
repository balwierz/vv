// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Sources layered on another source (--expand, --flatten, --gt-stats,
// VCF sample columns) and open_source(), which applies them.

#include "internal.hpp"

// ── Packed key=value columns (VCF INFO, GFF/GTF attributes) ─────────────────
// These live ABOVE the VV_CORE_LIB guard on purpose. They used to sit inside
// the ncurses frontend, which meant the TUI could show INFO as virtual columns
// while every export path (--tsv/--json/--parquet/--arrow), libvvcore and the
// Qt GUI saw only the raw blob. ExpandedSource below needs them too.

// Parse a VCF-INFO / GFF-attributes style key=value list. Handles "k=v;k=v"
// (VCF/GFF3) and 'k "v"; k "v";' (GTF). Bare tokens become flags with empty value.
std::vector<std::pair<std::string,std::string>>
parse_kv_list(const std::string& s) {
    std::vector<std::pair<std::string,std::string>> out;
    auto is_sp = [](char c){ return c == ' ' || c == '\t'; };
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (is_sp(s[i]) || s[i] == ';')) ++i;
        if (i >= s.size()) break;
        size_t ks = i;
        while (i < s.size() && s[i] != '=' && s[i] != ';' && !is_sp(s[i])) ++i;
        std::string key = s.substr(ks, i - ks);
        if (key.empty()) { ++i; continue; }
        while (i < s.size() && (s[i] == '=' || is_sp(s[i]))) ++i;
        std::string value;
        if (i < s.size() && s[i] == '"') {
            ++i;
            size_t vs = i;
            while (i < s.size() && s[i] != '"') ++i;
            value = s.substr(vs, i - vs);
            if (i < s.size()) ++i;
        } else if (i < s.size() && s[i] != ';') {
            size_t vs = i;
            while (i < s.size() && s[i] != ';') ++i;
            value = s.substr(vs, i - vs);
            while (!value.empty() && is_sp(value.back())) value.pop_back();
        }
        out.emplace_back(std::move(key), std::move(value));
    }
    return out;
}

// Heuristic: does this cell look like a k=v;k=v list worth expanding?
bool looks_like_kv_list(const std::string& s) {
    return s.find(';') != std::string::npos &&
           (s.find('=') != std::string::npos || s.find('"') != std::string::npos);
}

// Parse VCF ##INFO=<ID=X,Number=...,Type=T,Description="..."> header lines.
// Returns (ID, Arrow type) pairs in file order. Type maps VCF types to Arrow:
//   Integer → INT64, Float → DOUBLE, Flag → BOOL, everything else → STRING.
std::vector<std::pair<std::string, arrow::Type::type>>
parse_vcf_info_headers(const std::vector<std::string>& preamble) {
    std::vector<std::pair<std::string, arrow::Type::type>> out;
    const std::string prefix = "##INFO=<";
    for (auto& line : preamble) {
        if (line.rfind(prefix, 0) != 0 || line.empty() || line.back() != '>') continue;
        std::string body = line.substr(prefix.size(), line.size() - prefix.size() - 1);
        std::string id, type, number;
        size_t i = 0, n = body.size();
        while (i < n) {
            size_t ke = body.find('=', i);
            if (ke == std::string::npos) break;
            std::string k = body.substr(i, ke - i);
            size_t vs = ke + 1, ve;
            std::string v;
            if (vs < n && body[vs] == '"') {
                ve = body.find('"', vs + 1);
                if (ve == std::string::npos) break;
                v = body.substr(vs + 1, ve - vs - 1);
                i = (ve + 1 < n) ? ve + 2 : n;  // skip closing quote + comma
            } else {
                ve = body.find(',', vs);
                if (ve == std::string::npos) ve = n;
                v = body.substr(vs, ve - vs);
                i = (ve < n) ? ve + 1 : n;
            }
            if (k == "ID")     id = v;
            if (k == "Type")   type = v;
            if (k == "Number") number = v;
        }
        if (id.empty()) continue;
        // Number is load-bearing: A / R / G / . mean "one value per allele /
        // per genotype / variable", so a declared Integer like AD or PL holds
        // "12,4" per record. Typing that INT64 makes every value null. Only
        // Number=1 (or a Flag, which has none) gets a scalar Arrow type.
        const bool scalar = (number == "1" || number.empty());
        arrow::Type::type t = arrow::Type::STRING;
        if      (type == "Flag")               t = arrow::Type::BOOL;
        else if (!scalar)                      t = arrow::Type::STRING;
        else if (type == "Integer")            t = arrow::Type::INT64;
        else if (type == "Float")              t = arrow::Type::DOUBLE;
        out.emplace_back(std::move(id), t);
    }
    return out;
}
// ── ExpandedSource: packed key=value column → real columns ───────────────────
//
// A decorator over another source. VCF INFO and GFF/GTF attributes carry the
// actual payload of those formats as one opaque string, so `--filter`,
// `--select`, `--parquet`, `--unique` and the Qt GUI could not see any of it.
// The TUI had its own display-only expansion; this replaces that with a real
// schema-level one that every consumer inherits.
//
// The expanded columns are APPENDED, so existing column indices are unchanged
// and the raw blob is still there — a projection or a region-column
// auto-detection that worked before keeps working.
class ExpandedSource : public TabularSource {
    std::unique_ptr<TabularSource>  inner_;
    int                             src_col_ = -1;   // the packed column
    std::vector<std::string>        keys_;           // appended, in order
    std::vector<arrow::Type::type>  types_;
    std::shared_ptr<arrow::Schema>  schema_;
    int                             n_inner_ = 0;

    // Build one expanded column's array from the packed strings of a chunk.
    arrow::Status build_key_array(const arrow::ChunkedArray& packed,
                                   size_t ki,
                                   std::shared_ptr<arrow::Array>* out) const {
        const std::string& key = keys_[ki];
        const arrow::Type::type t = types_[ki];
        arrow::StringBuilder sb;
        arrow::Int64Builder  ib;
        arrow::DoubleBuilder db;
        arrow::BooleanBuilder bb;
        for (const auto& chunk : packed.chunks()) {
            for (int64_t r = 0; r < chunk->length(); ++r) {
                bool found = false;
                std::string val;
                if (!chunk->IsNull(r)) {
                    const std::string raw = cell_to_string(*chunk, r);
                    if (raw != NULL_SYMBOL) {
                        for (auto& kv : parse_kv_list(raw)) {
                            if (kv.first == key) { found = true; val = kv.second; break; }
                        }
                    }
                }
                switch (t) {
                    case arrow::Type::BOOL:
                        // A VCF Flag is present-or-absent, never null.
                        ARROW_RETURN_NOT_OK(bb.Append(found));
                        break;
                    case arrow::Type::INT64:
                        if (!found || val.empty()) ARROW_RETURN_NOT_OK(ib.AppendNull());
                        else {
                            try { ARROW_RETURN_NOT_OK(ib.Append(std::stoll(val))); }
                            catch (...) { ARROW_RETURN_NOT_OK(ib.AppendNull()); }
                        }
                        break;
                    case arrow::Type::DOUBLE:
                        if (!found || val.empty()) ARROW_RETURN_NOT_OK(db.AppendNull());
                        else {
                            try { ARROW_RETURN_NOT_OK(db.Append(std::stod(val))); }
                            catch (...) { ARROW_RETURN_NOT_OK(db.AppendNull()); }
                        }
                        break;
                    default:
                        // A key that is absent is null; a bare flag-like key
                        // that is present with no value is an empty string.
                        if (!found) ARROW_RETURN_NOT_OK(sb.AppendNull());
                        else        ARROW_RETURN_NOT_OK(sb.Append(val));
                }
            }
        }
        switch (t) {
            case arrow::Type::BOOL:   return bb.Finish(out);
            case arrow::Type::INT64:  return ib.Finish(out);
            case arrow::Type::DOUBLE: return db.Finish(out);
            default:                  return sb.Finish(out);
        }
    }

public:
    // Column names that would collide with an inner field get a suffix rather
    // than silently shadowing it.
    static std::string open(std::unique_ptr<TabularSource> inner,
                             const std::string& col_name,
                             std::unique_ptr<TabularSource>* out) {
        auto in_schema = inner->schema();
        int idx = in_schema->GetFieldIndex(col_name);
        if (idx < 0)
            return "--expand: no column named '" + col_name + "'";
        auto t = in_schema->field(idx)->type()->id();
        if (t != arrow::Type::STRING && t != arrow::Type::LARGE_STRING)
            return "--expand: column '" + col_name + "' is " +
                   type_label(*in_schema->field(idx)->type()) +
                   ", not text — nothing to unpack";

        auto self = std::unique_ptr<ExpandedSource>(new ExpandedSource());
        self->src_col_ = idx;
        self->n_inner_ = in_schema->num_fields();

        // Key discovery. A VCF declares its INFO keys and their types in the
        // header, which is authoritative and cheap. GFF/GTF declares nothing,
        // so the keys come from the first chunk — see the caveat in --help:
        // a `-n 10` preview and a full scan CAN disagree on the schema.
        std::vector<std::pair<std::string, arrow::Type::type>> decl =
            parse_vcf_info_headers(inner->preamble_below());
        if (decl.empty())
            decl = parse_vcf_info_headers(inner->preamble_above());

        // The ##INFO declarations describe the VCF INFO column and nothing
        // else, so only that column may use them. Taking this branch for any
        // column of any VCF meant `--expand REF` / `--expand FILTER` skipped
        // the shape gate below and appended every declared key as an all-null
        // (all-false, for Flag) column, exit 0, no diagnostic — while
        // man/vv.1 promises such a column is refused. Anything else falls
        // through to discovery, which gates properly.
        const bool decl_applies =
            !decl.empty() && col_name.size() == 4 &&
            (col_name[0]=='I'||col_name[0]=='i') && (col_name[1]=='N'||col_name[1]=='n') &&
            (col_name[2]=='F'||col_name[2]=='f') && (col_name[3]=='O'||col_name[3]=='o');

        if (decl_applies) {
            for (auto& [k, ty] : decl) { self->keys_.push_back(k); self->types_.push_back(ty); }
        } else {
            inner->ensure(0);
            std::shared_ptr<arrow::Table> first;
            if (inner->num_chunks() > 0 &&
                inner->read_chunk(0, {idx}, &first).ok() && first &&
                first->num_columns() == 1) {
                std::set<std::string> seen;
                auto col = first->column(0);
                // Gate first. parse_kv_list() treats a bare token as a flag,
                // so without this a plain text column (BED's Name, say) would
                // manufacture one column per distinct value — 20 junk columns
                // from `--expand Name`. Require that most non-null cells
                // actually carry `=` or `;`.
                int64_t sampled = 0, kv_like = 0;
                for (const auto& chunk : col->chunks()) {
                    for (int64_t r = 0; r < chunk->length(); ++r) {
                        if (chunk->IsNull(r)) continue;
                        const std::string raw = cell_to_string(*chunk, r);
                        if (raw == NULL_SYMBOL) continue;
                        ++sampled;
                        if (raw.find('=') != std::string::npos ||
                            raw.find(';') != std::string::npos) ++kv_like;
                    }
                }
                if (sampled > 0 && kv_like * 2 < sampled)
                    return "--expand: column '" + col_name + "' does not look "
                           "like a key=value list (no '=' or ';' in most "
                           "values) — nothing to unpack";
                for (const auto& chunk : col->chunks()) {
                    for (int64_t r = 0; r < chunk->length(); ++r) {
                        if (chunk->IsNull(r)) continue;
                        const std::string raw = cell_to_string(*chunk, r);
                        if (raw == NULL_SYMBOL) continue;
                        for (auto& kv : parse_kv_list(raw)) {
                            // parse_kv_list returns duplicates (gencode repeats
                            // `tag=`); first occurrence wins, order is stable.
                            if (seen.insert(kv.first).second &&
                                (int)self->keys_.size() < kMaxExpandKeys) {
                                self->keys_.push_back(kv.first);
                                self->types_.push_back(arrow::Type::STRING);
                            }
                        }
                    }
                }
            }
        }
        if (self->keys_.empty())
            return "--expand: found no key=value pairs in column '" + col_name + "'";

        arrow::FieldVector fields;
        for (int i = 0; i < in_schema->num_fields(); ++i)
            fields.push_back(in_schema->field(i));
        for (size_t k = 0; k < self->keys_.size(); ++k) {
            std::string nm = self->keys_[k];
            if (in_schema->GetFieldIndex(nm) >= 0) nm += "_" + col_name;
            fields.push_back(arrow::field(nm, arrow_type_for_id(self->types_[k])));
        }
        self->schema_ = arrow::schema(fields);
        self->inner_  = std::move(inner);
        *out = std::move(self);
        return "";
    }

    static constexpr int kMaxExpandKeys = 256;

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        // Split the request. Any expanded column also needs the packed source
        // column read, even when the caller did not ask for it.
        std::vector<int> inner_req;
        bool want_expanded = false;
        for (int c : col_indices) {
            if (c < n_inner_) inner_req.push_back(c);
            else              want_expanded = true;
        }
        if (want_expanded &&
            std::find(inner_req.begin(), inner_req.end(), src_col_) == inner_req.end())
            inner_req.push_back(src_col_);

        std::shared_ptr<arrow::Table> in_tbl;
        ARROW_RETURN_NOT_OK(inner_->read_chunk(i, inner_req, &in_tbl));
        if (!in_tbl) { *out = nullptr; return arrow::Status::OK(); }

        auto pos_of = [&](int inner_idx) {
            for (size_t k = 0; k < inner_req.size(); ++k)
                if (inner_req[k] == inner_idx) return (int)k;
            return -1;
        };

        // Assemble in the caller's requested order — write_delimited and
        // friends index the result by position within col_indices.
        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        for (int c : col_indices) {
            if (c < n_inner_) {
                int p = pos_of(c);
                if (p < 0) return arrow::Status::Invalid("expand: lost inner column");
                fields.push_back(schema_->field(c));
                cols.push_back(in_tbl->column(p));
            } else {
                int p = pos_of(src_col_);
                if (p < 0) return arrow::Status::Invalid("expand: missing packed column");
                std::shared_ptr<arrow::Array> arr;
                ARROW_RETURN_NOT_OK(build_key_array(*in_tbl->column(p),
                                                     (size_t)(c - n_inner_), &arr));
                fields.push_back(schema_->field(c));
                cols.push_back(std::make_shared<arrow::ChunkedArray>(arr));
            }
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols, in_tbl->num_rows());
        return arrow::Status::OK();
    }

    // ── Everything else forwards. chunk boundaries are preserved, so the
    //    streaming-eviction contract, chunk_meta() and the exact region
    //    row-count contract all survive unchanged.
    int64_t total_rows()      const override { return inner_->total_rows(); }
    int     num_chunks()      const override { return inner_->num_chunks(); }
    ChunkMeta chunk_meta(int i) const override { return inner_->chunk_meta(i); }
    void    ensure(int i)           override { inner_->ensure(i); }
    void    set_retain_all(bool b)  override { inner_->set_retain_all(b); }
    bool    evicted_any()     const override { return inner_->evicted_any(); }
    arrow::Status read_status() const override { return inner_->read_status(); }
    bool    region_applied()  const override { return inner_->region_applied(); }
    bool    change_slice(int d, bool abs, int64_t t) override {
        return inner_->change_slice(d, abs, t);
    }
    const std::string& path() const override { return inner_->path(); }
    std::string tab_label()   const override { return inner_->tab_label(); }
    std::string created_by()  const override { return inner_->created_by(); }
    std::string top_banner()  const override { return inner_->top_banner(); }
    std::vector<std::string> preamble_above() const override {
        return inner_->preamble_above();
    }
    std::vector<std::string> preamble_below() const override {
        return inner_->preamble_below();
    }
    std::vector<std::string> hidden_for_display() const override {
        return inner_->hidden_for_display();
    }
    bool show_cells_in_full() const override { return inner_->show_cells_in_full(); }
    std::string format_cell(int col_idx, std::string val) const override {
        return (col_idx < n_inner_) ? inner_->format_cell(col_idx, std::move(val))
                                     : val;
    }
    int min_col_width(int col_idx) const override {
        return (col_idx < n_inner_) ? inner_->min_col_width(col_idx) : 4;
    }
    std::string footer() const override {
        return inner_->footer() + "  |  expanded " +
               schema_->field(src_col_)->name() + " → " +
               std::to_string(keys_.size()) + " columns";
    }
};

bool is_expanded_source(const TabularSource& src) {
    return dynamic_cast<const ExpandedSource*>(&src) != nullptr;
}

// ── --flatten: struct columns as one column per leaf ────────────────────────
//
// A struct column (Parquet / Arrow / JSON nesting) becomes one column per
// leaf, named with the path ("st.a", "st.b.c"), recursively. Lists and maps
// keep their shape; other columns pass through. A struct that is null makes
// its leaves null (StructArray::GetFlattenedField merges the validity), so
// --filter / --sort / --describe / the TUI work on the leaves like any column.
class FlattenSource : public TabularSource {
    struct Flat { int inner; std::vector<int> path; };
    std::unique_ptr<TabularSource> inner_;
    std::shared_ptr<arrow::Schema> schema_;
    std::vector<Flat>              flat_;

    void add(const std::shared_ptr<arrow::Field>& f, const std::string& prefix, int inner,
             std::vector<int> path, arrow::FieldVector* out) {
        const std::string name = prefix.empty() ? f->name() : prefix + "." + f->name();
        if (f->type()->id() == arrow::Type::STRUCT) {
            for (int k = 0; k < f->type()->num_fields(); ++k) {
                auto p = path; p.push_back(k);
                add(f->type()->field(k), name, inner, std::move(p), out);
            }
            return;
        }
        out->push_back(f->WithName(name)->WithNullable(true));
        flat_.push_back({inner, std::move(path)});
    }

public:
    // Wrap `inner`; a schema without a struct column is returned unwrapped.
    static void open(std::unique_ptr<TabularSource> inner, std::unique_ptr<TabularSource>* out) {
        auto sch = inner->schema();
        bool any = false;
        for (const auto& f : sch->fields()) any |= f->type()->id() == arrow::Type::STRUCT;
        if (!any) { *out = std::move(inner); return; }
        auto self = std::make_unique<FlattenSource>();
        arrow::FieldVector fields;
        for (int i = 0; i < sch->num_fields(); ++i) self->add(sch->field(i), "", i, {}, &fields);
        self->schema_ = arrow::schema(fields);
        self->inner_ = std::move(inner);
        *out = std::move(self);
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                             std::shared_ptr<arrow::Table>* out) override {
        std::vector<int> need;                       // inner columns, first use order
        for (int c : col_indices)
            if (std::find(need.begin(), need.end(), flat_[(size_t)c].inner) == need.end())
                need.push_back(flat_[(size_t)c].inner);
        std::shared_ptr<arrow::Table> in;
        ARROW_RETURN_NOT_OK(inner_->read_chunk(i, need, &in));
        if (!in) { *out = nullptr; return arrow::Status::OK(); }
        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        for (int c : col_indices) {
            const Flat& fl = flat_[(size_t)c];
            const int pos = (int)(std::find(need.begin(), need.end(), fl.inner) - need.begin());
            arrow::ArrayVector parts;
            for (const auto& ch : in->column(pos)->chunks()) {
                std::shared_ptr<arrow::Array> a = ch;
                for (int k : fl.path) {
                    ARROW_ASSIGN_OR_RAISE(a, static_cast<const arrow::StructArray&>(*a)
                                                 .GetFlattenedField(k));
                }
                parts.push_back(std::move(a));
            }
            fields.push_back(schema_->field(c));
            cols.push_back(std::make_shared<arrow::ChunkedArray>(parts, schema_->field(c)->type()));
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols, in->num_rows());
        return arrow::Status::OK();
    }
    int64_t total_rows()      const override { return inner_->total_rows(); }
    int     num_chunks()      const override { return inner_->num_chunks(); }
    ChunkMeta chunk_meta(int i) const override { return inner_->chunk_meta(i); }
    void    ensure(int i)           override { inner_->ensure(i); }
    void    set_retain_all(bool b)  override { inner_->set_retain_all(b); }
    bool    evicted_any()     const override { return inner_->evicted_any(); }
    arrow::Status read_status() const override { return inner_->read_status(); }
    bool    region_applied()  const override { return inner_->region_applied(); }
    const std::string& path() const override { return inner_->path(); }
    std::string tab_label()   const override { return inner_->tab_label(); }
    std::string created_by()  const override { return inner_->created_by(); }
    std::string top_banner()  const override { return inner_->top_banner(); }
    std::vector<std::string> preamble_above() const override { return inner_->preamble_above(); }
    std::vector<std::string> preamble_below() const override { return inner_->preamble_below(); }
    std::vector<std::string> hidden_for_display() const override {
        return inner_->hidden_for_display();
    }
    bool show_cells_in_full() const override { return inner_->show_cells_in_full(); }
    std::string format_cell(int col_idx, std::string val) const override {
        const Flat& fl = flat_[(size_t)col_idx];
        return fl.path.empty() ? inner_->format_cell(fl.inner, std::move(val)) : val;
    }
    int min_col_width(int col_idx) const override {
        const Flat& fl = flat_[(size_t)col_idx];
        return fl.path.empty() ? inner_->min_col_width(fl.inner) : 4;
    }
    std::string footer() const override {
        return inner_->footer() + "  |  structs flattened \xe2\x86\x92 " +
               std::to_string(schema_->num_fields()) + " columns";
    }
};

// ── VCF genotype aggregates (--gt-stats) ──────────────────────────────────────
//
// Per-variant summaries over the per-sample GT field. A cohort VCF/BCF carries
// N samples × M FORMAT fields of genotype data; expanding all of it would be
// thousands of columns. Instead --gt-stats adds a FIXED set of columns —
// hom-ref / het / hom-alt / missing sample counts and pooled allele totals —
// that answer the usual cohort questions (call rate, allele frequency, how many
// carriers) regardless of sample count. Streaming: computed per chunk, so a
// cohort VCF larger than memory still previews.

namespace gtstat {

enum class GtClass { HomRef, Het, HomAlt, Missing };
struct GtInfo { GtClass cls; int an; int ac; };  // an: called alleles, ac: alt

// Classify one GT string: "0/1", "1|1", "./.", "0", "0/0/1", "1/2" (compound
// het). Diploid, haploid, and polyploid are all handled; a genotype with no
// present allele is Missing.
static GtInfo classify_gt(const std::string& gt) {
    GtInfo r{GtClass::Missing, 0, 0};
    bool all_zero = true, all_same_alt = true;
    int first_alt = -1;
    size_t i = 0, n = gt.size();
    while (i < n) {
        size_t j = i;
        while (j < n && gt[j] != '/' && gt[j] != '|') ++j;
        std::string tok = gt.substr(i, j - i);
        i = (j < n) ? j + 1 : n;
        if (tok == "." || tok.empty()) continue;      // a missing allele
        int a = 0;
        try { a = std::stoi(tok); } catch (...) { continue; }
        ++r.an;
        if (a != 0) all_zero = false;
        if (a > 0) {
            ++r.ac;
            if (first_alt < 0) first_alt = a;
            else if (a != first_alt) all_same_alt = false;
        }
    }
    if (r.an == 0)                          r.cls = GtClass::Missing;
    else if (all_zero)                      r.cls = GtClass::HomRef;
    else if (r.ac == r.an && all_same_alt)  r.cls = GtClass::HomAlt;
    else                                    r.cls = GtClass::Het;
    return r;
}

// Position of the GT sub-field in a FORMAT string ("GT:AD:DP" → 0). -1 if none.
static int gt_field_index(const std::string& fmt) {
    int idx = 0;
    size_t i = 0, n = fmt.size();
    while (i <= n) {
        size_t j = i;
        while (j < n && fmt[j] != ':') ++j;
        if (fmt.compare(i, j - i, "GT") == 0) return idx;
        if (j >= n) break;
        i = j + 1; ++idx;
    }
    return -1;
}

// The `idx`-th colon-separated sub-field of a sample string ("0/1:5,6:11", 0 →
// "0/1"). Empty when the field is absent.
static std::string nth_subfield(const std::string& s, int idx) {
    size_t i = 0, n = s.size();
    int k = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && s[j] != ':') ++j;
        if (k == idx) return s.substr(i, j - i);
        if (j >= n) break;
        i = j + 1; ++k;
    }
    return std::string();
}

}  // namespace gtstat

class GenotypeStatsSource : public TabularSource {
    std::unique_ptr<TabularSource> inner_;
    std::shared_ptr<arrow::Schema> schema_;
    int                            n_inner_ = 0;
    // Source columns holding genotype text. BCF collapses FORMAT + all samples
    // into one tab-joined "FORMAT_SAMPLES" column; VCF text keeps FORMAT and the
    // sample columns separate.
    bool                           collapsed_ = false;
    int                            fs_col_    = -1;   // collapsed: FORMAT_SAMPLES
    int                            fmt_col_   = -1;   // separate: FORMAT
    int                            n_samples_ = 0;    // separate: fixed sample count
    std::vector<int>               samp_cols_;        // separate: the sample columns
    std::vector<int>               src_cols_;         // inner cols needed to compute

    static constexpr int kNStat = 9;   // the appended columns
    static const char* stat_name(int k) {
        static const char* names[kNStat] = {
            "n_called", "n_het", "n_hom_ref", "n_hom_alt", "n_missing",
            "AC", "AN", "AF", "call_rate"};
        return names[k];
    }

    // Compute the kNStat aggregate arrays for one chunk. `col_at` returns the
    // ChunkedArray in `tbl` for an inner column index.
    arrow::Status build_stat_arrays(
            const arrow::Table& tbl,
            const std::function<std::shared_ptr<arrow::ChunkedArray>(int)>& col_at,
            std::vector<std::shared_ptr<arrow::Array>>* out) const {
        int64_t nrows = tbl.num_rows();
        arrow::Int64Builder  called_b, het_b, hr_b, ha_b, miss_b, ac_b, an_b;
        arrow::DoubleBuilder af_b, cr_b;

        // Row-major access to the source columns via a flat cache.
        auto cell = [](const std::shared_ptr<arrow::ChunkedArray>& c, int64_t r) {
            // ChunkedArray row lookup.
            int64_t rr = r;
            for (const auto& ch : c->chunks()) {
                if (rr < ch->length())
                    return ch->IsNull(rr) ? std::string() : cell_to_string(*ch, rr);
                rr -= ch->length();
            }
            return std::string();
        };
        std::shared_ptr<arrow::ChunkedArray> fs, fmt;
        std::vector<std::shared_ptr<arrow::ChunkedArray>> samp;
        if (collapsed_) fs = col_at(fs_col_);
        else {
            fmt = col_at(fmt_col_);
            for (int c : samp_cols_) samp.push_back(col_at(c));
        }

        std::vector<std::string> sample_gts;
        for (int64_t r = 0; r < nrows; ++r) {
            std::string fmt_s;
            sample_gts.clear();
            if (collapsed_) {
                const std::string blob = cell(fs, r);
                // FORMAT<TAB>S1<TAB>S2...
                size_t i = 0, n = blob.size(); bool first = true;
                while (i <= n) {
                    size_t j = i;
                    while (j < n && blob[j] != '\t') ++j;
                    std::string tok = blob.substr(i, j - i);
                    if (first) { fmt_s = tok; first = false; }
                    else       sample_gts.push_back(std::move(tok));
                    if (j >= n) break;
                    i = j + 1;
                }
            } else {
                fmt_s = cell(fmt, r);
                for (auto& sc : samp) sample_gts.push_back(cell(sc, r));
            }

            int gi = gtstat::gt_field_index(fmt_s);
            int n_samp = (int)sample_gts.size();
            int64_t het = 0, hr = 0, ha = 0, miss = 0, ac = 0, an = 0;
            if (gi < 0) {
                // No GT field this row: every sample counts as missing.
                miss = n_samp;
            } else {
                for (const auto& s : sample_gts) {
                    gtstat::GtInfo g = gtstat::classify_gt(gtstat::nth_subfield(s, gi));
                    switch (g.cls) {
                        case gtstat::GtClass::HomRef:  ++hr;   break;
                        case gtstat::GtClass::Het:     ++het;  break;
                        case gtstat::GtClass::HomAlt:  ++ha;   break;
                        case gtstat::GtClass::Missing: ++miss; break;
                    }
                    ac += g.ac; an += g.an;
                }
            }
            int64_t called = hr + het + ha;
            ARROW_RETURN_NOT_OK(called_b.Append(called));
            ARROW_RETURN_NOT_OK(het_b.Append(het));
            ARROW_RETURN_NOT_OK(hr_b.Append(hr));
            ARROW_RETURN_NOT_OK(ha_b.Append(ha));
            ARROW_RETURN_NOT_OK(miss_b.Append(miss));
            ARROW_RETURN_NOT_OK(ac_b.Append(ac));
            ARROW_RETURN_NOT_OK(an_b.Append(an));
            if (an > 0) ARROW_RETURN_NOT_OK(af_b.Append((double)ac / (double)an));
            else        ARROW_RETURN_NOT_OK(af_b.AppendNull());
            if (n_samp > 0) ARROW_RETURN_NOT_OK(cr_b.Append((double)called / (double)n_samp));
            else            ARROW_RETURN_NOT_OK(cr_b.AppendNull());
        }
        out->resize(kNStat);
        ARROW_RETURN_NOT_OK(called_b.Finish(&(*out)[0]));
        ARROW_RETURN_NOT_OK(het_b.Finish(&(*out)[1]));
        ARROW_RETURN_NOT_OK(hr_b.Finish(&(*out)[2]));
        ARROW_RETURN_NOT_OK(ha_b.Finish(&(*out)[3]));
        ARROW_RETURN_NOT_OK(miss_b.Finish(&(*out)[4]));
        ARROW_RETURN_NOT_OK(ac_b.Finish(&(*out)[5]));
        ARROW_RETURN_NOT_OK(an_b.Finish(&(*out)[6]));
        ARROW_RETURN_NOT_OK(af_b.Finish(&(*out)[7]));
        ARROW_RETURN_NOT_OK(cr_b.Finish(&(*out)[8]));
        return arrow::Status::OK();
    }

public:
    // `samples`: the sample column names (vcf_sample_names() of the source as
    // read). Columns appended after them (--expand) are not samples; without
    // the list every column after FORMAT is taken as one.
    static std::string open(std::unique_ptr<TabularSource> inner,
                            std::unique_ptr<TabularSource>* out,
                            const std::vector<std::string>& samples = {}) {
        auto in_schema = inner->schema();
        auto self = std::unique_ptr<GenotypeStatsSource>(new GenotypeStatsSource());
        self->n_inner_ = in_schema->num_fields();

        int fs = in_schema->GetFieldIndex("FORMAT_SAMPLES");
        int fmt = in_schema->GetFieldIndex("FORMAT");
        if (fs >= 0) {
            self->collapsed_ = true;
            self->fs_col_ = fs;
            self->src_cols_ = {fs};
        } else if (fmt >= 0 && fmt + 1 < self->n_inner_) {
            self->collapsed_ = false;
            self->fmt_col_ = fmt;
            for (const auto& nm : samples) {
                const int c = in_schema->GetFieldIndex(nm);
                if (c > fmt) self->samp_cols_.push_back(c);
            }
            if (samples.empty())
                for (int c = fmt + 1; c < self->n_inner_; ++c) self->samp_cols_.push_back(c);
            self->n_samples_ = (int)self->samp_cols_.size();
            self->src_cols_.push_back(fmt);
            for (int c : self->samp_cols_) self->src_cols_.push_back(c);
        } else {
            return "--gt-stats: no per-sample genotypes found — needs a VCF/BCF "
                   "with a FORMAT column and one or more sample columns";
        }

        arrow::FieldVector fields;
        for (int i = 0; i < self->n_inner_; ++i) fields.push_back(in_schema->field(i));
        for (int k = 0; k < kNStat; ++k) {
            std::string nm = stat_name(k);
            if (in_schema->GetFieldIndex(nm) >= 0) nm += "_gt";   // avoid a clash
            auto ty = (k <= 6) ? arrow::int64() : arrow::float64();
            fields.push_back(arrow::field(nm, ty));
        }
        self->schema_ = arrow::schema(fields);
        self->inner_ = std::move(inner);
        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        // Any stat column needs every genotype source column read.
        std::vector<int> inner_req;
        bool want_stat = false;
        for (int c : col_indices) {
            if (c < n_inner_) inner_req.push_back(c);
            else              want_stat = true;
        }
        if (want_stat)
            for (int sc : src_cols_)
                if (std::find(inner_req.begin(), inner_req.end(), sc) == inner_req.end())
                    inner_req.push_back(sc);

        std::shared_ptr<arrow::Table> in_tbl;
        ARROW_RETURN_NOT_OK(inner_->read_chunk(i, inner_req, &in_tbl));
        if (!in_tbl) { *out = nullptr; return arrow::Status::OK(); }

        auto pos_of = [&](int inner_idx) {
            for (size_t k = 0; k < inner_req.size(); ++k)
                if (inner_req[k] == inner_idx) return (int)k;
            return -1;
        };

        std::vector<std::shared_ptr<arrow::Array>> stats;
        if (want_stat) {
            auto col_at = [&](int inner_idx) {
                return in_tbl->column(pos_of(inner_idx));
            };
            ARROW_RETURN_NOT_OK(build_stat_arrays(*in_tbl, col_at, &stats));
        }

        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        for (int c : col_indices) {
            fields.push_back(schema_->field(c));
            if (c < n_inner_) {
                cols.push_back(in_tbl->column(pos_of(c)));
            } else {
                cols.push_back(std::make_shared<arrow::ChunkedArray>(
                    stats[(size_t)(c - n_inner_)]));
            }
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols, in_tbl->num_rows());
        return arrow::Status::OK();
    }

    // Everything else forwards, preserving chunk boundaries (streaming contract).
    int64_t total_rows()      const override { return inner_->total_rows(); }
    int     num_chunks()      const override { return inner_->num_chunks(); }
    ChunkMeta chunk_meta(int i) const override { return inner_->chunk_meta(i); }
    void    ensure(int i)           override { inner_->ensure(i); }
    void    set_retain_all(bool b)  override { inner_->set_retain_all(b); }
    bool    evicted_any()     const override { return inner_->evicted_any(); }
    arrow::Status read_status() const override { return inner_->read_status(); }
    bool    region_applied()  const override { return inner_->region_applied(); }
    const std::string& path() const override { return inner_->path(); }
    std::vector<std::string> preamble_above() const override {
        return inner_->preamble_above();
    }
    std::vector<std::string> preamble_below() const override {
        return inner_->preamble_below();
    }
    std::vector<std::string> hidden_for_display() const override {
        return inner_->hidden_for_display();
    }
    bool show_cells_in_full() const override { return inner_->show_cells_in_full(); }
    std::string format_cell(int col_idx, std::string val) const override {
        return (col_idx < n_inner_) ? inner_->format_cell(col_idx, std::move(val))
                                     : val;
    }
    std::string footer() const override {
        return inner_->footer() + "  |  +genotype stats";
    }
};

// The dispatch ladder. open_source() wraps this so a decorator (--expand)
// applies to every one of its ~25 success paths at once, including the
// multi-file TUI loop, instead of each `*out = std::move(src)` needing a patch.
// ── VCF / BCF sample columns: typed structs, or one row per sample ──────────
//
// A multi-sample VCF stores, per record, a FORMAT key list ("GT:AD:DP") and one
// packed value per sample ("0/1:5,6:11"). By default (--samples struct) each
// sample column becomes a struct typed from the ##FORMAT declarations —
// {GT: 0/1, AD: [5, 6], DP: 11}; keys a record lacks are null (and left out
// when the struct is shown) and the FORMAT column, now redundant, is dropped.
// --samples long instead emits one row per record × sample: the record's
// columns, `sample`, then one typed column per FORMAT key, so
// `--filter 'DP < 10'` applies across samples. --samples text keeps the packed
// strings. Types: Integer / Float with Number=1 → int64 / double, with any
// other Number (A, R, G, ., n) → a list of them; String / Character → text.

enum class FmtKind { Str, Int, Float, IntList, FloatList };
struct FmtDef { std::string id; FmtKind kind; };

static std::vector<FmtDef> parse_vcf_format_headers(const std::vector<std::string>& preamble) {
    std::vector<FmtDef> out;
    const std::string prefix = "##FORMAT=<";
    for (const auto& line : preamble) {
        if (line.rfind(prefix, 0) != 0 || line.back() != '>') continue;
        const std::string body = line.substr(prefix.size(), line.size() - prefix.size() - 1);
        std::string id, type, number;
        size_t i = 0, n = body.size();
        while (i < n) {
            size_t ke = body.find('=', i);
            if (ke == std::string::npos) break;
            const std::string k = body.substr(i, ke - i);
            size_t vs = ke + 1, ve;
            std::string v;
            if (vs < n && body[vs] == '"') {
                ve = body.find('"', vs + 1);
                if (ve == std::string::npos) break;
                v = body.substr(vs + 1, ve - vs - 1);
                i = (ve + 1 < n) ? ve + 2 : n;
            } else {
                ve = body.find(',', vs);
                if (ve == std::string::npos) ve = n;
                v = body.substr(vs, ve - vs);
                i = (ve < n) ? ve + 1 : n;
            }
            if (k == "ID") id = v; else if (k == "Type") type = v; else if (k == "Number") number = v;
        }
        if (id.empty()) continue;
        bool dup = false;
        for (const auto& d : out) dup |= d.id == id;
        if (dup) continue;
        const bool scalar = number == "1";
        FmtKind kind = FmtKind::Str;
        if (type == "Integer") kind = scalar ? FmtKind::Int : FmtKind::IntList;
        else if (type == "Float") kind = scalar ? FmtKind::Float : FmtKind::FloatList;
        out.push_back({id, kind});
    }
    return out;
}

static std::shared_ptr<arrow::DataType> fmt_arrow_type(FmtKind k) {
    switch (k) {
        case FmtKind::Int:       return arrow::int64();
        case FmtKind::Float:     return arrow::float64();
        case FmtKind::IntList:   return arrow::list(arrow::int64());
        case FmtKind::FloatList: return arrow::list(arrow::float64());
        default:                 return arrow::utf8();
    }
}

// The FORMAT column and the sample columns after it, when `sch` is a VCF's
// (CHROM .. INFO, FORMAT, samples). Empty when there are no samples.
static std::vector<std::string> vcf_sample_names(const arrow::Schema& sch) {
    std::vector<std::string> out;
    if (sch.num_fields() < 10) return out;
    const std::string c0 = sch.field(0)->name();
    if ((c0 != "CHROM" && c0 != "#CHROM") || sch.field(8)->name() != "FORMAT") return out;
    for (int i = 9; i < sch.num_fields(); ++i) out.push_back(sch.field(i)->name());
    return out;
}

class VcfSamplesSource : public TabularSource {
    // Output column → what it is built from.
    struct OutCol { enum Kind { Pass, Sample, SampleName, Field } kind; int inner; int idx; };
    std::unique_ptr<TabularSource>        inner_;
    bool                                  long_ = false;
    int                                   fmt_col_ = -1;
    std::vector<int>                      samp_cols_;     // inner sample columns
    std::vector<std::string>              samp_names_;
    std::vector<FmtDef>                   defs_;
    std::unordered_map<std::string, int>  def_index_;
    std::vector<OutCol>                   out_;
    std::shared_ptr<arrow::Schema>        schema_;
    std::shared_ptr<arrow::DataType>      struct_type_;

    int64_t nsamp() const { return (int64_t)samp_cols_.size(); }

    static std::string cell_at(const arrow::ChunkedArray& c, int64_t r) {
        for (const auto& ch : c.chunks()) {
            if (r < ch->length()) return ch->IsNull(r) ? std::string() : cell_to_string(*ch, r);
            r -= ch->length();
        }
        return {};
    }

    // A text column of one chunk, read as string_views (no copy per cell).
    struct Strs {
        std::shared_ptr<arrow::Array> arr;
        const arrow::StringArray*     s = nullptr;
        std::string                   tmp;
        std::string_view get(int64_t r) {
            if (arr->IsNull(r)) return {};
            if (s) return s->GetView(r);
            tmp = cell_to_string(*arr, r);
            return tmp;
        }
    };
    static arrow::Status make_strs(const arrow::ChunkedArray& c, Strs* out) {
        if (c.num_chunks() == 1) out->arr = c.chunk(0);
        else if (c.num_chunks() == 0) { ARROW_ASSIGN_OR_RAISE(out->arr, arrow::MakeArrayOfNull(c.type(), 0)); }
        else { ARROW_ASSIGN_OR_RAISE(out->arr, arrow::Concatenate(c.chunks())); }
        if (out->arr->type_id() == arrow::Type::STRING)
            out->s = static_cast<const arrow::StringArray*>(out->arr.get());
        return arrow::Status::OK();
    }

    // Append one FORMAT value (token) of kind `k` to `b`; "." or "" is null.
    static arrow::Status append_value(arrow::ArrayBuilder* b, FmtKind k, std::string_view tok) {
        if (tok.empty() || tok == ".") return b->AppendNull();
        switch (k) {
            case FmtKind::Int: {
                long long v = 0;
                auto r = std::from_chars(tok.data(), tok.data() + tok.size(), v);
                auto* ib = static_cast<arrow::Int64Builder*>(b);
                return (r.ec == std::errc() && r.ptr == tok.data() + tok.size()) ? ib->Append(v)
                                                                                : ib->AppendNull();
            }
            case FmtKind::Float: {
                double v = 0;
                auto r = std::from_chars(tok.data(), tok.data() + tok.size(), v);
                auto* db = static_cast<arrow::DoubleBuilder*>(b);
                return (r.ec == std::errc() && r.ptr == tok.data() + tok.size()) ? db->Append(v)
                                                                                : db->AppendNull();
            }
            case FmtKind::IntList:
            case FmtKind::FloatList: {
                auto* lb = static_cast<arrow::ListBuilder*>(b);
                ARROW_RETURN_NOT_OK(lb->Append());
                const FmtKind ek = k == FmtKind::IntList ? FmtKind::Int : FmtKind::Float;
                size_t i = 0;
                while (i <= tok.size()) {
                    size_t j = tok.find(',', i);
                    if (j == std::string_view::npos) j = tok.size();
                    ARROW_RETURN_NOT_OK(append_value(lb->value_builder(), ek, tok.substr(i, j - i)));
                    i = j + 1;
                }
                return arrow::Status::OK();
            }
            default:
                return static_cast<arrow::StringBuilder*>(b)->Append(tok);
        }
    }

    // FORMAT keys of one record → def index per position (-1: undeclared).
    std::vector<int> key_defs(const std::string& fmt) const {
        std::vector<int> out;
        size_t i = 0;
        while (i <= fmt.size() && !fmt.empty()) {
            size_t j = fmt.find(':', i);
            if (j == std::string::npos) j = fmt.size();
            auto it = def_index_.find(fmt.substr(i, j - i));
            out.push_back(it == def_index_.end() ? -1 : it->second);
            i = j + 1;
        }
        return out;
    }

    // Split one sample's packed value by ':' into the per-def tokens (views
    // into `v`; empty when the record or the sample lacks the key).
    void split_sample(std::string_view v, const std::vector<int>& kd,
                      std::vector<std::string_view>* tok, std::vector<char>* has) const {
        tok->assign(defs_.size(), std::string_view());
        has->assign(defs_.size(), 0);
        size_t i = 0, p = 0;
        while (i <= v.size() && !v.empty() && p < kd.size()) {
            size_t j = v.find(':', i);
            if (j == std::string_view::npos) j = v.size();
            if (kd[p] >= 0 && !(*has)[(size_t)kd[p]]) {
                (*tok)[(size_t)kd[p]] = v.substr(i, j - i);
                (*has)[(size_t)kd[p]] = 1;
            }
            ++p;
            i = j + 1;
        }
    }

public:
    static std::unique_ptr<TabularSource> wrap(std::unique_ptr<TabularSource> inner,
                                               const std::vector<std::string>& samples,
                                               bool long_form) {
        auto sch = inner->schema();
        const int fmt = sch->GetFieldIndex("FORMAT");
        if (fmt < 0 || samples.empty()) return inner;
        auto self = std::make_unique<VcfSamplesSource>();
        self->long_ = long_form;
        self->fmt_col_ = fmt;
        for (const auto& n : samples) {
            const int c = sch->GetFieldIndex(n);
            if (c <= fmt) return inner;              // not the layout we expect
            self->samp_cols_.push_back(c);
            self->samp_names_.push_back(n);
        }
        self->defs_ = parse_vcf_format_headers(inner->preamble_below());
        if (self->defs_.empty()) self->defs_ = parse_vcf_format_headers(inner->preamble_above());
        if (self->defs_.empty()) {
            // No ##FORMAT lines: take the keys of the first chunk, as text.
            inner->ensure(0);
            std::shared_ptr<arrow::Table> t;
            if (inner->num_chunks() > 0 && inner->read_chunk(0, {fmt}, &t).ok() && t) {
                for (int64_t r = 0; r < t->num_rows(); ++r) {
                    const std::string f = cell_at(*t->column(0), r);
                    size_t i = 0;
                    while (i <= f.size() && !f.empty()) {
                        size_t j = f.find(':', i);
                        if (j == std::string::npos) j = f.size();
                        const std::string k = f.substr(i, j - i);
                        bool seen = false;
                        for (const auto& d : self->defs_) seen |= d.id == k;
                        if (!seen && !k.empty()) self->defs_.push_back({k, FmtKind::Str});
                        i = j + 1;
                    }
                }
            }
        }
        if (self->defs_.empty()) return inner;
        for (size_t k = 0; k < self->defs_.size(); ++k) self->def_index_[self->defs_[k].id] = (int)k;
        arrow::FieldVector sfields;
        for (const auto& d : self->defs_) sfields.push_back(arrow::field(d.id, fmt_arrow_type(d.kind)));
        self->struct_type_ = arrow::struct_(sfields);

        auto is_sample = [&](int c) {
            return std::find(self->samp_cols_.begin(), self->samp_cols_.end(), c) != self->samp_cols_.end();
        };
        arrow::FieldVector fields;
        if (!long_form) {
            for (int i = 0; i < sch->num_fields(); ++i) {
                if (i == fmt) continue;
                if (is_sample(i)) {
                    self->out_.push_back({OutCol::Sample, i, -1});
                    fields.push_back(arrow::field(sch->field(i)->name(), self->struct_type_));
                } else {
                    self->out_.push_back({OutCol::Pass, i, -1});
                    fields.push_back(sch->field(i));
                }
            }
        } else {
            for (int i = 0; i < fmt; ++i) {
                self->out_.push_back({OutCol::Pass, i, -1});
                fields.push_back(sch->field(i));
            }
            self->out_.push_back({OutCol::SampleName, -1, -1});
            fields.push_back(arrow::field("sample", arrow::utf8()));
            for (size_t k = 0; k < self->defs_.size(); ++k) {
                std::string name = self->defs_[k].id;
                if (sch->GetFieldIndex(name) >= 0 || name == "sample") name += "_fmt";
                self->out_.push_back({OutCol::Field, -1, (int)k});
                fields.push_back(arrow::field(name, fmt_arrow_type(self->defs_[k].kind)));
            }
            for (int i = fmt + 1; i < sch->num_fields(); ++i) {
                if (is_sample(i)) continue;
                self->out_.push_back({OutCol::Pass, i, -1});
                fields.push_back(sch->field(i));
            }
        }
        self->schema_ = arrow::schema(fields);
        self->inner_ = std::move(inner);
        return self;
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                             std::shared_ptr<arrow::Table>* out) override {
        // Inner columns needed: the passed-through ones, plus FORMAT and the
        // samples whenever a sample-derived column is asked for.
        std::vector<int> need;
        auto want = [&](int c) {
            if (std::find(need.begin(), need.end(), c) == need.end()) need.push_back(c);
        };
        bool any_field = false;
        for (int c : col_indices) {
            const OutCol& oc = out_[(size_t)c];
            if (oc.kind == OutCol::Pass) want(oc.inner);
            else if (oc.kind == OutCol::Sample) { want(fmt_col_); want(oc.inner); }
            else if (oc.kind == OutCol::Field) { any_field = true; }
        }
        if (any_field) { want(fmt_col_); for (int sc : samp_cols_) want(sc); }
        if (need.empty()) want(fmt_col_);                 // for the row count
        std::shared_ptr<arrow::Table> in;
        ARROW_RETURN_NOT_OK(inner_->read_chunk(i, need, &in));
        if (!in) { *out = nullptr; return arrow::Status::OK(); }
        auto col_of = [&](int inner_c) {
            return in->column((int)(std::find(need.begin(), need.end(), inner_c) - need.begin()));
        };
        const int64_t n = in->num_rows();
        const int64_t S = long_ ? nsamp() : 1;
        std::vector<std::vector<int>> kd;                 // per row: key → def
        auto keys = [&]() -> const std::vector<std::vector<int>>& {
            if (kd.empty() && n > 0) {
                auto f = col_of(fmt_col_);
                kd.resize((size_t)n);
                for (int64_t r = 0; r < n; ++r) kd[(size_t)r] = key_defs(cell_at(*f, r));
            }
            return kd;
        };
        // Long form: every sample-derived column for this chunk in one pass.
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> field_b(defs_.size());
        std::shared_ptr<arrow::Array> name_arr;
        if (long_ && any_field) {
            for (size_t k = 0; k < defs_.size(); ++k)
                ARROW_RETURN_NOT_OK(arrow::MakeBuilder(arrow::default_memory_pool(),
                                                       fmt_arrow_type(defs_[k].kind), &field_b[k]));
            std::vector<Strs> sc(samp_cols_.size());
            for (size_t k = 0; k < samp_cols_.size(); ++k)
                ARROW_RETURN_NOT_OK(make_strs(*col_of(samp_cols_[k]), &sc[k]));
            const auto& K = keys();
            std::vector<std::string_view> tok;
            std::vector<char> has;
            for (int64_t r = 0; r < n; ++r)
                for (int64_t s = 0; s < S; ++s) {
                    split_sample(sc[(size_t)s].get(r), K[(size_t)r], &tok, &has);
                    for (size_t k = 0; k < defs_.size(); ++k)
                        ARROW_RETURN_NOT_OK(has[k] ? append_value(field_b[k].get(), defs_[k].kind, tok[k])
                                                   : field_b[k]->AppendNull());
                }
        }
        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::Array>> cols;
        for (int c : col_indices) {
            const OutCol& oc = out_[(size_t)c];
            fields.push_back(schema_->field(c));
            std::shared_ptr<arrow::Array> arr;
            if (oc.kind == OutCol::Pass) {
                auto ca = col_of(oc.inner);
                if (!long_) {
                    if (ca->num_chunks() == 0) {
                        ARROW_ASSIGN_OR_RAISE(arr, arrow::MakeArrayOfNull(ca->type(), 0));
                    } else {
                        ARROW_ASSIGN_OR_RAISE(arr, arrow::Concatenate(ca->chunks()));
                    }
                } else {                                  // each row once per sample
                    std::unique_ptr<arrow::ArrayBuilder> b;
                    ARROW_RETURN_NOT_OK(arrow::MakeBuilder(arrow::default_memory_pool(), ca->type(), &b));
                    ARROW_RETURN_NOT_OK(b->Reserve(n * S));
                    for (const auto& ch : ca->chunks()) {
                        arrow::ArraySpan span(*ch->data());
                        for (int64_t r = 0; r < ch->length(); ++r)
                            for (int64_t s = 0; s < S; ++s)
                                ARROW_RETURN_NOT_OK(b->AppendArraySlice(span, r, 1));
                    }
                    ARROW_RETURN_NOT_OK(b->Finish(&arr));
                }
            } else if (oc.kind == OutCol::SampleName) {
                arrow::StringBuilder b;
                for (int64_t r = 0; r < n; ++r)
                    for (int64_t s = 0; s < S; ++s) ARROW_RETURN_NOT_OK(b.Append(samp_names_[(size_t)s]));
                ARROW_RETURN_NOT_OK(b.Finish(&arr));
            } else if (oc.kind == OutCol::Field) {
                // Finish each builder once; a column asked for twice reuses it.
                if (field_b[(size_t)oc.idx]) {
                    ARROW_RETURN_NOT_OK(field_b[(size_t)oc.idx]->Finish(&arr));
                    field_b[(size_t)oc.idx].reset();
                    name_arr = arr;                       // keep for a repeat
                } else {
                    arr = name_arr;
                }
            } else {                                      // Sample → struct
                std::unique_ptr<arrow::ArrayBuilder> ub;
                ARROW_RETURN_NOT_OK(arrow::MakeBuilder(arrow::default_memory_pool(), struct_type_, &ub));
                auto* sb = static_cast<arrow::StructBuilder*>(ub.get());
                Strs sc;
                ARROW_RETURN_NOT_OK(make_strs(*col_of(oc.inner), &sc));
                const auto& K = keys();
                std::vector<std::string_view> tok;
                std::vector<char> has;
                for (int64_t r = 0; r < n; ++r) {
                    const std::string_view v = sc.get(r);
                    if (v.empty() || v == ".") {
                        for (size_t k = 0; k < defs_.size(); ++k)
                            ARROW_RETURN_NOT_OK(sb->field_builder((int)k)->AppendNull());
                        ARROW_RETURN_NOT_OK(sb->Append(false));
                        continue;
                    }
                    ARROW_RETURN_NOT_OK(sb->Append(true));
                    split_sample(v, K[(size_t)r], &tok, &has);
                    for (size_t k = 0; k < defs_.size(); ++k)
                        ARROW_RETURN_NOT_OK(has[k] ? append_value(sb->field_builder((int)k), defs_[k].kind, tok[k])
                                                   : sb->field_builder((int)k)->AppendNull());
                }
                ARROW_RETURN_NOT_OK(sb->Finish(&arr));
            }
            cols.push_back(std::move(arr));
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols, n * S);
        return arrow::Status::OK();
    }

    int64_t total_rows() const override {
        const int64_t t = inner_->total_rows();
        return (t >= 0 && long_) ? t * nsamp() : t;
    }
    int     num_chunks() const override { return inner_->num_chunks(); }
    ChunkMeta chunk_meta(int i) const override {
        ChunkMeta m = inner_->chunk_meta(i);
        if (long_) { m.first_row *= nsamp(); m.num_rows *= nsamp(); }
        return m;
    }
    void    ensure(int i)           override { inner_->ensure(i); }
    void    set_retain_all(bool b)  override { inner_->set_retain_all(b); }
    bool    evicted_any()     const override { return inner_->evicted_any(); }
    arrow::Status read_status() const override { return inner_->read_status(); }
    bool    region_applied()  const override { return inner_->region_applied(); }
    const std::string& path() const override { return inner_->path(); }
    std::string tab_label()   const override { return inner_->tab_label(); }
    std::string created_by()  const override { return inner_->created_by(); }
    std::string top_banner()  const override { return inner_->top_banner(); }
    std::vector<std::string> preamble_above() const override { return inner_->preamble_above(); }
    std::vector<std::string> preamble_below() const override { return inner_->preamble_below(); }
    std::vector<std::string> hidden_for_display() const override {
        return inner_->hidden_for_display();
    }
    bool show_cells_in_full() const override { return inner_->show_cells_in_full(); }
    std::string format_cell(int col_idx, std::string val) const override {
        const OutCol& oc = out_[(size_t)col_idx];
        return oc.kind == OutCol::Pass ? inner_->format_cell(oc.inner, std::move(val)) : val;
    }
    int min_col_width(int col_idx) const override {
        const OutCol& oc = out_[(size_t)col_idx];
        return oc.kind == OutCol::Pass ? inner_->min_col_width(oc.inner) : 4;
    }
    std::string footer() const override {
        return inner_->footer() + (long_ ? "  |  one row per record \xc3\x97 sample"
                                         : "  |  sample columns typed from ##FORMAT");
    }
};

// --contigs reads only a genomics file's header and presents its reference
// sequences as a table; defined below (near main) but reachable here so every
// caller — CLI, GUI, KDE plugins — gets it through open_source().

// The public entry point: dispatch, then apply --expand once. Doing it here
// rather than at each `*out = std::move(src)` means every format and every
// caller (CLI, multi-file TUI loop, Qt GUI, KDE plugins) gets it, and no
// future dispatch branch can forget to.
std::string open_source(const std::string& path, const Config& cfg,
                         std::unique_ptr<TabularSource>* out) {
    // Arrow's CPU and IO thread pools are function-local statics created on
    // first use, so they are destroyed at exit in reverse order of creation.
    // The streaming CSV reader creates the IO pool first; the CPU pool would
    // then be destroyed first, and a read-ahead still in flight at exit (a
    // preview that stopped reading a large file) hands its result to a dead
    // executor — a crash or a hang in exit(). Creating the CPU pool before any
    // reader exists makes it outlive the IO pool. The CLI already does this by
    // sizing the pool in main(); the GUI and the KDE plugins reach here first.
    static const int cpu_pool_first = arrow::GetCpuThreadPoolCapacity();
    (void)cpu_pool_first;
    // --contigs replaces the data view with the header's reference-sequence
    // table (BAM/CRAM/SAM, VCF/BCF), reading no records.
    if (cfg.contigs) return build_contigs(cfg, out);
    if (cfg.seq_stats) return build_seq_stats(cfg, out);
    std::string err = open_source_dispatch(path, cfg, out);
    if (!err.empty() || !*out) return err;
    // Order matters. The VCF sample columns are named as read, before
    // --expand appends columns after them; --gt-stats parses the samples'
    // packed text, so it runs before they become structs (or rows); --flatten
    // runs last, so it turns sample structs into S1.GT, S1.DP, ...
    const std::vector<std::string> samples = vcf_sample_names(*(*out)->schema());
    if (!cfg.expand_col.empty()) {
        std::unique_ptr<TabularSource> wrapped;
        err = ExpandedSource::open(std::move(*out), cfg.expand_col, &wrapped);
        if (!err.empty()) return err;
        *out = std::move(wrapped);
    }
    if (cfg.gt_stats) {
        std::unique_ptr<TabularSource> wrapped;
        err = GenotypeStatsSource::open(std::move(*out), &wrapped, samples);
        if (!err.empty()) return err;
        *out = std::move(wrapped);
    }
    if (!samples.empty() && cfg.samples != "text")
        *out = VcfSamplesSource::wrap(std::move(*out), samples, cfg.samples == "long");
    if (cfg.flatten) {
        std::unique_ptr<TabularSource> wrapped;
        FlattenSource::open(std::move(*out), &wrapped);
        *out = std::move(wrapped);
    }
    return "";
}
