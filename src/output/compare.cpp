// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// --check: do genomic files agree on their reference sequences (names,
// naming style, lengths, assembly)?

#include "internal.hpp"

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace {

// One file's reference sequences, and where vv found them.
struct ContigSet {
    std::string              path;
    std::string              source;      // "header", "index (.fai)", "data (column 'chrom')"
    std::vector<std::string> names;       // in the file's order
    std::vector<int64_t>     lengths;     // per name; <= 0 when not stated
    std::string              assembly;
};

bool ends_with_ci(const std::string& s, const char* suffix) { return fends_ci(s, suffix); }

// Names (and lengths) from a FASTA's .fai: name<TAB>length<TAB>...
bool read_fai(const std::string& fai, ContigSet* out) {
    std::ifstream in(fai);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        const size_t t = line.find('\t');
        if (t == std::string::npos) continue;
        out->names.push_back(line.substr(0, t));
        out->lengths.push_back(std::strtoll(line.c_str() + t + 1, nullptr, 10));
    }
    return !out->names.empty();
}

// The distinct values of a source's chromosome column, in order of first
// appearance (BED, GFF, a TSV or Parquet with a chrom column, ...).
std::string scan_chrom_column(const std::string& path, const Config& cfg, ContigSet* out) {
    Config c = cfg;
    c.path = path;
    c.region.clear();
    c.filter_expr.clear();
    c.select_cols.clear();
    std::unique_ptr<TabularSource> src;
    if (auto e = open_source(path, c, &src); !e.empty()) return e;
    const auto schema = src->schema();
    const LocusColumns loc = locus_columns(*schema, format_label_of(*src));
    if (loc.chrom < 0)
        return "no reference sequences: no header lists them and no chromosome column was found";
    std::unordered_set<std::string> seen;
    for (int ch = 0;; ++ch) {
        src->ensure(ch);
        if (ch >= src->num_chunks()) break;
        std::shared_ptr<arrow::Table> t;
        if (auto st = src->read_chunk(ch, {loc.chrom}, &t); !st.ok() || !t) {
            note_chunk_error(st);
            continue;
        }
        for (const auto& arr : t->column(0)->chunks())
            for (int64_t r = 0; r < arr->length(); ++r) {
                if (arr->IsNull(r)) continue;
                std::string v = cell_to_string(*arr, r);
                if (seen.insert(v).second) {
                    out->names.push_back(v);
                    out->lengths.push_back(0);
                }
            }
    }
    if (auto st = run_read_status(*src); !st.ok()) return shorten_reader_error(st.ToString());
    out->source = "data (column '" + schema->field(loc.chrom)->name() + "')";
    return "";
}

std::string collect_contigs(const std::string& path, const Config& cfg, ContigSet* out) {
    out->path = path;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !is_remote_url(path))
        return "no such file";
    const bool hts = ends_with_ci(path, ".bam") || ends_with_ci(path, ".cram") ||
                     ends_with_ci(path, ".sam") || ends_with_ci(path, ".vcf") ||
                     ends_with_ci(path, ".vcf.gz") || ends_with_ci(path, ".vcf.bgz") ||
                     ends_with_ci(path, ".bcf");
    if (hts) {
        GenomicHeader h;
        const int level = hts_get_log_level();
        hts_set_log_level(HTS_LOG_ERROR);     // header lint (Number=A …) is not this check's business
        const std::string e = read_genomic_header(path, cfg.pileup_ref, &h);
        hts_set_log_level((htsLogLevel)level);
        if (!e.empty()) return e;
        if (!h.contig_names.empty()) {
            out->names   = h.contig_names;
            out->lengths = h.contig_lengths;
            out->source  = "header";
        }
    } else if (ends_with_ci(path, ".bw") || ends_with_ci(path, ".bigwig") ||
               ends_with_ci(path, ".bb") || ends_with_ci(path, ".bigbed")) {
        if (!big_chrom_list(path, &out->names, &out->lengths)) return "cannot read its chromosome list";
        out->source = "header";
    } else if (ends_with_ci(path, ".2bit")) {
        Config c = cfg;
        c.path = path;
        std::unique_ptr<TabularSource> src;
        if (auto e = open_source(path, c, &src); !e.empty()) return e;
        const auto sch = src->schema();
        const int ni = sch->GetFieldIndex("name"), li = sch->GetFieldIndex("length");
        if (ni < 0 || li < 0) return "no sequence index";
        for (int ch = 0;; ++ch) {
            src->ensure(ch);
            if (ch >= src->num_chunks()) break;
            std::shared_ptr<arrow::Table> t;
            if (!src->read_chunk(ch, {ni, li}, &t).ok() || !t) continue;
            for (int64_t r = 0; r < t->num_rows(); ++r) {
                double len = 0;
                out->names.push_back(cell_to_string(*t->column(0)->chunk(0), r));
                cell_as_double(*t, 1, r, &len);
                out->lengths.push_back((int64_t)len);
            }
        }
        out->source = "index";
    } else if (read_fai(path + ".fai", out)) {
        out->source = "index (.fai)";
    } else if (tabix_seqnames(path, &out->names)) {
        out->lengths.assign(out->names.size(), 0);
        out->source = "index (tabix)";
    }
    if (out->names.empty()) {
        out->lengths.clear();
        if (auto e = scan_chrom_column(path, cfg, out); !e.empty()) return e;
    }
    out->assembly = detect_assembly(out->names, out->lengths);
    return "";
}

// "chr" when the standard chromosomes are named chr1 / chrX, "plain" for
// 1 / X, "" when the file names none of them (scaffolds only).
std::string naming_style(const ContigSet& s) {
    int chr = 0, plain = 0;
    for (const auto& n : s.names) {
        if (chrom_alias(n).empty()) continue;      // not a standard chromosome
        (n.rfind("chr", 0) == 0 ? chr : plain)++;
    }
    if (chr == 0 && plain == 0) return "";
    return chr >= plain ? "chr" : "plain";
}

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

std::string list_some(const std::vector<std::string>& v, size_t max = 5) {
    std::string o;
    for (size_t i = 0; i < v.size() && i < max; ++i) o += (i ? ", " : "") + v[i];
    if (v.size() > max) o += ", … (" + std::to_string(v.size()) + " in all)";
    return o;
}

}  // namespace

std::string run_check(const Config& cfg, bool* compatible) {
    *compatible = true;
    const std::vector<std::string>& paths = cfg.paths;
    if (paths.size() < 2) return "--check compares two or more files; give the reference first";
    std::vector<ContigSet> sets(paths.size());
    for (size_t i = 0; i < paths.size(); ++i)
        if (auto e = collect_contigs(paths[i], cfg, &sets[i]); !e.empty())
            return paths[i] + ": " + e;

    const ContigSet& ref = sets[0];
    std::unordered_map<std::string, size_t> ref_at;
    for (size_t i = 0; i < ref.names.size(); ++i) ref_at.emplace(ref.names[i], i);
    const std::string ref_style = naming_style(ref);

    struct Result { std::vector<std::string> problems; };
    std::vector<Result> res(sets.size());
    for (size_t f = 1; f < sets.size(); ++f) {
        const ContigSet& s = sets[f];
        auto& probs = res[f].problems;
        const std::string style = naming_style(s);
        if (!style.empty() && !ref_style.empty() && style != ref_style)
            probs.push_back("naming: " + display_basename(s.path) + " writes " +
                            (style == "chr" ? "chr1" : "1") + ", " + display_basename(ref.path) +
                            " " + (ref_style == "chr" ? "chr1" : "1"));
        std::vector<std::string> missing, lengths;
        for (size_t i = 0; i < s.names.size(); ++i) {
            auto it = ref_at.find(s.names[i]);
            if (it == ref_at.end()) {
                const std::string alias = chrom_alias(s.names[i]);
                if (!alias.empty()) it = ref_at.find(alias);
            }
            if (it == ref_at.end()) { missing.push_back(s.names[i]); continue; }
            const int64_t a = ref.lengths[it->second], b = s.lengths[i];
            if (a > 0 && b > 0 && a != b)
                lengths.push_back(s.names[i] + " " + std::to_string(b) + " vs " + std::to_string(a));
        }
        if (!missing.empty())
            probs.push_back(std::to_string(missing.size()) + " not in " + display_basename(ref.path) +
                            ": " + list_some(missing));
        if (!lengths.empty())
            probs.push_back(std::to_string(lengths.size()) + " of another length: " + list_some(lengths));
        if (!s.assembly.empty() && !ref.assembly.empty() && s.assembly != ref.assembly)
            probs.push_back("assembly: " + s.assembly + ", " + display_basename(ref.path) + " " +
                            ref.assembly);
        if (!probs.empty()) *compatible = false;
    }

    if (cfg.json_array || cfg.json_lines) {
        std::string o = "{\"reference\": " + json_str(display_path(ref.path)) +
                        ", \"compatible\": " + (*compatible ? "true" : "false") + ", \"files\": [";
        for (size_t f = 0; f < sets.size(); ++f) {
            const ContigSet& s = sets[f];
            const std::string style = naming_style(s);
            o += std::string(f ? ", " : "") + "{\"path\": " + json_str(display_path(s.path)) +
                 ", \"source\": " + json_str(s.source) +
                 ", \"contigs\": " + std::to_string(s.names.size()) +
                 ", \"naming\": " + (style.empty() ? "null" : json_str(style)) +
                 ", \"assembly\": " + (s.assembly.empty() ? "null" : json_str(s.assembly)) +
                 ", \"problems\": [";
            for (size_t p = 0; p < res[f].problems.size(); ++p)
                o += (p ? ", " : "") + json_str(res[f].problems[p]);
            o += "]}";
        }
        std::printf("%s]}\n", o.c_str());
        return "";
    }
    int w = 0;
    for (const auto& s : sets) w = std::max(w, display_width(display_basename(s.path)));
    for (size_t f = 0; f < sets.size(); ++f) {
        const ContigSet& s = sets[f];
        const std::string name = display_basename(s.path);
        const std::string style = naming_style(s);
        std::string line = name + std::string((size_t)(w - display_width(name) + 2), ' ') +
                           std::to_string(s.names.size()) + " sequences from the " + s.source;
        if (!style.empty()) line += ", " + std::string(style == "chr" ? "chr1" : "1") + " naming";
        if (!s.assembly.empty()) line += ", " + s.assembly;
        if (f == 0) line += "  (the reference)";
        else line += res[f].problems.empty() ? "  ok" : "  MISMATCH";
        std::printf("%s\n", line.c_str());
        for (const auto& p : res[f].problems) std::printf("    %s\n", p.c_str());
    }
    return "";
}

// ── --diff A B ───────────────────────────────────────────────────────────────
//
// Two tables compared as their values read: the columns only in one, the
// row counts, and the cells of the columns both have, row by row in file
// order (no key, no join). Cells compare as the text the exporters write
// (floats shortest round-trip, so 0.5 in a CSV equals 0.5 in a Parquet
// double), a null apart from any text. A column of another type or in
// another order is noted; the values decide whether the tables are the same.

namespace {

// Rows of one source, read chunk by chunk through --filter, as text cells of
// `cols`.
class RowCursor {
public:
    RowCursor(TabularSource& src, std::vector<int> cols, const FilterExpr* fx)
        : src_(src), cols_(std::move(cols)), fx_(fx) {
        read_set_ = fx_ ? union_with_filter(cols_, *fx_) : cols_;
        for (int c : cols_)
            pos_.push_back((int)(std::find(read_set_.begin(), read_set_.end(), c) - read_set_.begin()));
    }
    // The next row's cells; false at the end.
    bool next(std::vector<std::string>* cells) {
        while (!t_ || r_ >= t_->num_rows()) {
            src_.ensure(chunk_);
            if (chunk_ >= src_.num_chunks()) return false;
            std::shared_ptr<arrow::Table> t;
            const auto st = fx_ ? read_filtered_chunk(src_, chunk_, read_set_, *fx_, &t)
                                : src_.read_chunk(chunk_, read_set_, &t);
            ++chunk_;
            if (!st.ok()) { note_chunk_error(st); continue; }
            if (!t) continue;
            auto one = t->CombineChunks();
            if (!one.ok()) { note_chunk_error(one.status()); continue; }
            t_ = *one;
            r_ = 0;
        }
        cells->resize(cols_.size());
        for (size_t k = 0; k < cols_.size(); ++k) {
            std::string& s = (*cells)[k];
            s.clear();
            const auto& col = t_->column(pos_[k]);
            const arrow::Array& a = *col->chunk(0);
            if (a.IsNull(r_)) s = std::string("\0null", 5);
            else append_cell(a, r_, s);
        }
        ++r_;
        return true;
    }

private:
    TabularSource&                src_;
    std::vector<int>              cols_, read_set_, pos_;
    const FilterExpr*             fx_;
    int                           chunk_ = 0;
    std::shared_ptr<arrow::Table> t_;
    int64_t                       r_ = 0;
};

std::string shown(const std::string& cell) {
    return cell == std::string("\0null", 5) ? std::string("null") : cell;
}

}  // namespace

std::string run_diff(const Config& cfg, bool* same) {
    *same = true;
    if (cfg.paths.size() != 2) return "--diff compares two files";
    ExactFloats exact;   // cells as the exporters write them, floats lossless
    std::unique_ptr<TabularSource> src[2];
    for (int i = 0; i < 2; ++i) {
        Config c = cfg;
        c.path = cfg.paths[(size_t)i];
        if (auto e = open_source(c.path, c, &src[i]); !e.empty())
            return cfg.paths[(size_t)i] + ": " + e;
        if (!cfg.tab.empty())
            if (auto e = select_tab(src[i], cfg.tab); !e.empty()) return cfg.paths[(size_t)i] + ": " + e;
    }
    // The columns: --select applied to each side by name.
    std::vector<int> cols[2];
    std::shared_ptr<arrow::Schema> sch[2];
    FilterExpr fx[2];
    for (int i = 0; i < 2; ++i) {
        sch[i] = src[i]->schema();
        std::vector<std::string> unknown;
        cols[i] = select_field_indices(*src[i], cfg, &unknown, true);
        if (!unknown.empty())
            return cfg.paths[(size_t)i] + ": " + unknown_columns_error(*src[i], unknown);
        if (!cfg.filter_expr.empty()) {
            std::string ferr;
            if (!parse_filter_expr(cfg.filter_expr, *sch[i], &fx[i], &ferr))
                return cfg.paths[(size_t)i] + ": --filter: " + ferr;
        }
    }
    const std::string an = display_basename(cfg.paths[0]), bn = display_basename(cfg.paths[1]);
    auto name = [&](int i, int c) { return sch[i]->field(c)->name(); };
    std::vector<std::string> only_a, only_b, types, a_order, b_order;
    std::vector<int> common_a, common_b;
    for (int c : cols[0]) {
        int match = -1;
        for (int d : cols[1]) if (name(1, d) == name(0, c)) { match = d; break; }
        if (match < 0) { only_a.push_back(name(0, c)); continue; }
        common_a.push_back(c);
        common_b.push_back(match);
        a_order.push_back(name(0, c));
        const std::string ta = type_label(*sch[0]->field(c)->type());
        const std::string tb = type_label(*sch[1]->field(match)->type());
        if (ta != tb) types.push_back(name(0, c) + ": " + ta + " in " + an + ", " + tb + " in " + bn);
    }
    for (int d : cols[1]) {
        bool found = false;
        for (int c : cols[0]) if (name(0, c) == name(1, d)) { found = true; break; }
        if (!found) only_b.push_back(name(1, d));
        else b_order.push_back(name(1, d));
    }
    const bool reordered = a_order != b_order;

    // The rows.
    const int64_t show = cfg.head_rows_set && cfg.head_rows > 0 ? (int64_t)cfg.head_rows : 10;
    RowCursor ra(*src[0], common_a, cfg.filter_expr.empty() ? nullptr : &fx[0]);
    RowCursor rb(*src[1], common_b, cfg.filter_expr.empty() ? nullptr : &fx[1]);
    struct Cell { int64_t row; std::string col, a, b; };
    std::vector<Cell> cells;
    int64_t rows_a = 0, rows_b = 0, differing = 0, cells_differing = 0;
    std::vector<std::string> va, vb;
    for (;;) {
        const bool ha = ra.next(&va), hb = rb.next(&vb);
        if (ha) ++rows_a;
        if (hb) ++rows_b;
        if (!ha || !hb) {
            // Count the rest of the longer side.
            while (ha && ra.next(&va)) ++rows_a;
            while (hb && rb.next(&vb)) ++rows_b;
            break;
        }
        bool row_differs = false;
        for (size_t k = 0; k < va.size(); ++k) {
            if (va[k] == vb[k]) continue;
            row_differs = true;
            ++cells_differing;
            if ((int64_t)cells.size() < show) cells.push_back({rows_a, a_order[k], shown(va[k]), shown(vb[k])});
        }
        if (row_differs) ++differing;
    }
    for (int i = 0; i < 2; ++i)
        if (auto st = run_read_status(*src[i]); !st.ok())
            return cfg.paths[(size_t)i] + ": " + shorten_reader_error(st.ToString());
    // The values decide; a type or an order a format imposes (a CSV has no
    // float32) is noted, not a difference.
    *same = only_a.empty() && only_b.empty() && rows_a == rows_b && differing == 0;

    if (cfg.json_array || cfg.json_lines) {
        auto arr = [&](const std::vector<std::string>& v) {
            std::string o = "[";
            for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + json_str(v[i]);
            return o + "]";
        };
        std::string o = "{\"same\": " + std::string(*same ? "true" : "false") +
                        ", \"a\": " + json_str(display_path(cfg.paths[0])) +
                        ", \"b\": " + json_str(display_path(cfg.paths[1])) +
                        ", \"only_in_a\": " + arr(only_a) + ", \"only_in_b\": " + arr(only_b) +
                        ", \"type_changes\": " + arr(types) +
                        ", \"reordered\": " + (reordered ? "true" : "false") +
                        ", \"rows_a\": " + std::to_string(rows_a) +
                        ", \"rows_b\": " + std::to_string(rows_b) +
                        ", \"rows_differing\": " + std::to_string(differing) +
                        ", \"cells_differing\": " + std::to_string(cells_differing) +
                        ", \"first_differences\": [";
        for (size_t i = 0; i < cells.size(); ++i)
            o += std::string(i ? ", " : "") + "{\"row\": " + std::to_string(cells[i].row) +
                 ", \"column\": " + json_str(cells[i].col) + ", \"a\": " + json_str(cells[i].a) +
                 ", \"b\": " + json_str(cells[i].b) + "}";
        std::printf("%s]}\n", o.c_str());
        return "";
    }
    if (*same) std::printf("same: %lld rows, %zu columns\n", (long long)rows_a, common_a.size());
    if (!only_a.empty()) std::printf("columns only in %s: %s\n", an.c_str(), list_some(only_a, 20).c_str());
    if (!only_b.empty()) std::printf("columns only in %s: %s\n", bn.c_str(), list_some(only_b, 20).c_str());
    for (const auto& t : types) std::printf("note: type of %s\n", t.c_str());
    if (reordered) std::printf("note: the columns are in another order\n");
    if (*same) return "";
    if (rows_a != rows_b)
        std::printf("rows: %lld in %s, %lld in %s\n", (long long)rows_a, an.c_str(),
                    (long long)rows_b, bn.c_str());
    if (differing > 0) {
        std::printf("rows that differ: %lld of %lld compared (%lld cells)\n", (long long)differing,
                    (long long)std::min(rows_a, rows_b), (long long)cells_differing);
        std::printf("row\tcolumn\t%s\t%s\n", an.c_str(), bn.c_str());
        for (const auto& c : cells)
            std::printf("%lld\t%s\t%s\t%s\n", (long long)c.row, c.col.c_str(), c.a.c_str(), c.b.c_str());
        if (cells_differing > (int64_t)cells.size())
            std::printf("… %lld more (-n N shows more)\n", (long long)(cells_differing - (int64_t)cells.size()));
    }
    return "";
}
