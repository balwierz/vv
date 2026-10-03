// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// GenBank (.gb .gbk .gbff .genbank) and EMBL (.embl) flat files: one or more
// records, each a header, a feature table and an optional sequence. The two
// share the feature-table layout (key at column 6, location / qualifiers at
// column 22). Tabs: features (one row per feature, qualifiers as columns),
// records (one row per record) and sequences. The file is parsed into memory.

#include "internal.hpp"

namespace {

struct Feature {
    std::string type, location;
    std::vector<std::pair<std::string, std::string>> quals;
    bool open = false;           // the last qualifier's quoted value is not closed
};

struct Record {
    std::string locus, accession, version, molecule, topology, division, date;
    std::string definition, organism, taxonomy, keywords, sequence;
    int64_t     length = -1;
    std::vector<Feature> features;
    std::string id() const {
        return !version.empty() ? version : !accession.empty() ? accession : locus;
    }
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    std::istringstream in(s);
    for (std::string t; in >> t;) w.push_back(t);
    return w;
}

void append(std::string& dst, const std::string& more, const char* sep = " ") {
    if (more.empty()) return;
    if (!dst.empty()) dst += sep;
    dst += more;
}

std::string strip_final_dot(std::string s) {
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// Sequence letters of an ORIGIN / SQ line (digits, spaces dropped).
void add_sequence(std::string& seq, const std::string& line) {
    for (char c : line)
        if (std::isalpha((unsigned char)c) || c == '*' || c == '-') seq += c;
}

// A feature-table line (GenBank: 5 spaces; EMBL: "FT" + 3 spaces): a new
// feature (key at column 6) or a continuation (column 22): a qualifier, or
// more of the location / the open qualifier value.
void feature_line(Record& r, const std::string& line) {
    if (line.size() > 5 && line[5] != ' ') {
        Feature f;
        const size_t sp = line.find(' ', 5);
        f.type = line.substr(5, sp == std::string::npos ? std::string::npos : sp - 5);
        f.location = sp == std::string::npos ? "" : trim(line.substr(sp));
        r.features.push_back(std::move(f));
        return;
    }
    if (r.features.empty()) return;
    Feature& f = r.features.back();
    const std::string t = trim(line.size() > 5 ? line.substr(5) : "");
    if (t.empty()) return;
    if (t[0] == '/' && !f.open) {
        const size_t eq = t.find('=');
        std::string key = t.substr(1, eq == std::string::npos ? std::string::npos : eq - 1);
        std::string val = eq == std::string::npos ? "" : t.substr(eq + 1);
        f.open = !val.empty() && val[0] == '"' &&
                 (val.size() == 1 || std::count(val.begin(), val.end(), '"') % 2 == 1);
        f.quals.emplace_back(std::move(key), std::move(val));
    } else if (f.quals.empty()) {
        f.location += t;                         // a location wrapped over lines
    } else {
        auto& [key, val] = f.quals.back();
        // Text wraps at a space; a protein translation wraps mid-sequence.
        if (key != "translation" && !val.empty()) val += ' ';
        val += t;
        if (f.open) f.open = std::count(val.begin(), val.end(), '"') % 2 == 1;
    }
}

// A finished qualifier value: quotes removed, "" unescaped.
std::string qual_value(const std::string& v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); ++i) {
            out += v[i];
            if (v[i] == '"' && v[i + 1] == '"') ++i;
        }
        return out;
    }
    return v;
}

// 0-based half-open span and strand of a location such as
// complement(join(<1..20,30..>40)); remote parts (ACC:1..5) are skipped.
void location_span(const std::string& loc, int64_t* start, int64_t* end, char* strand) {
    *start = -1; *end = -1;
    std::string s;
    for (char c : loc) if (c != '<' && c != '>' && c != ' ') s += c;
    // Strand: the whole location complemented, or every part of a join / order.
    auto parts = [](const std::string& x) {
        std::vector<std::string> v;
        int depth = 0;
        size_t from = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            if (x[i] == '(') ++depth;
            else if (x[i] == ')') --depth;
            else if (x[i] == ',' && depth == 0) { v.push_back(x.substr(from, i - from)); from = i + 1; }
        }
        v.push_back(x.substr(from));
        return v;
    };
    *strand = '+';
    if (s.rfind("complement(", 0) == 0) {
        *strand = '-';
    } else if (s.rfind("join(", 0) == 0 || s.rfind("order(", 0) == 0) {
        const size_t o = s.find('(');
        const auto ps = parts(s.substr(o + 1, s.size() - o - 2));
        bool all = !ps.empty();
        for (const auto& p : ps) all = all && p.rfind("complement(", 0) == 0;
        if (all) *strand = '-';
    }
    // Spans: numbers outside remote references.
    std::string flat;
    for (char c : s) flat += (c == '(' || c == ')') ? ',' : c;
    for (const std::string& p : parts(flat)) {
        if (p.empty() || p.find(':') != std::string::npos) continue;
        const char* q = p.c_str();
        if (!std::isdigit((unsigned char)*q)) continue;   // a function name left over
        char* e = nullptr;
        const long long a = std::strtoll(q, &e, 10);
        long long b = a;
        if (e && (std::strncmp(e, "..", 2) == 0 || *e == '^' || *e == '.')) {
            const char* r = e + (std::strncmp(e, "..", 2) == 0 ? 2 : 1);
            if (std::isdigit((unsigned char)*r)) b = std::strtoll(r, nullptr, 10);
            if (*e == '^') b = a;                          // between two bases
        }
        if (a < 1 || b < a) continue;
        if (*start < 0 || a - 1 < *start) *start = a - 1;
        if (b > *end) *end = b;
    }
}

// Parse a GenBank or EMBL file. "" or an error.
std::string parse(const std::string& path, bool embl, std::vector<Record>* out) {
    std::shared_ptr<arrow::io::InputStream> in;
    if (auto e = open_decoded_file(path, &in); !e.empty()) return e;
    LineReader lr(in);
    std::string line;
    Record cur;
    bool in_rec = false, in_seq = false, in_feat = false;
    std::string field;                    // GenBank header field a continuation belongs to
    int64_t line_no = 0;
    auto finish = [&]() {
        for (Feature& f : cur.features) f.open = false;
        out->push_back(std::move(cur));
        cur = Record{};
        in_rec = in_seq = in_feat = false;
        field.clear();
    };
    while (true) {
        const bool nl = lr.read_line(&line);
        if (!nl && line.empty()) break;
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("//", 0) == 0) { if (in_rec) finish(); continue; }
        if (embl) {
            if (line.size() < 2) continue;
            const std::string code = line.substr(0, 2);
            const std::string rest = line.size() > 5 ? trim(line.substr(5)) : "";
            if (code == "ID") {
                if (in_rec) finish();
                in_rec = true;
                // ID   X56734; SV 1; linear; mRNA; STD; PLN; 1859 BP.
                std::vector<std::string> f;
                std::stringstream ss(rest);
                for (std::string t; std::getline(ss, t, ';');) f.push_back(trim(t));
                // Pre-2006 form: ID   AA03518    standard; DNA; FUN; 237 BP.
                const bool old = !f.empty() && f[0].find_first_of(" \t") != std::string::npos;
                if (!f.empty()) cur.accession = cur.locus = words(f[0])[0];
                for (size_t k = 1; k < f.size(); ++k) {
                    const std::string& t = f[k];
                    if (t.rfind("SV ", 0) == 0) cur.version = cur.accession + "." + trim(t.substr(3));
                    else if (t == "linear" || t == "circular") cur.topology = t;
                    else if (t.size() > 3 && (t.find(" BP") != std::string::npos ||
                                              t.find(" AA") != std::string::npos))
                        cur.length = std::strtoll(t.c_str(), nullptr, 10);
                    else if (k == (old ? 1u : 3u)) cur.molecule = t;
                    else if (k == (old ? 2u : 5u)) cur.division = t;
                }
                if (f.size() == 2 && cur.length < 0)       // ID   name  standard; DNA; ...
                    cur.length = std::strtoll(f.back().c_str(), nullptr, 10);
            } else if (!in_rec) {
                continue;
            } else if (in_seq) {
                add_sequence(cur.sequence, line);
            } else if (code == "AC") {
                if (cur.accession.empty() || cur.accession == cur.locus) {
                    const std::string a = trim(rest.substr(0, rest.find(';')));
                    if (!a.empty()) {
                        if (!cur.version.empty() && cur.version.rfind(cur.accession + ".", 0) == 0)
                            cur.version = a + cur.version.substr(cur.accession.size());
                        cur.accession = a;
                    }
                }
            } else if (code == "DE") append(cur.definition, rest);
            else if (code == "KW") {            // "k1; k2;" wrapped, or one per line
                const bool sep = !cur.keywords.empty() && cur.keywords.back() != ';';
                append(cur.keywords, strip_final_dot(rest), sep ? "; " : " ");
            }
            else if (code == "OS") { if (cur.organism.empty()) cur.organism = rest; }
            else if (code == "OC") append(cur.taxonomy, rest);
            else if (code == "DT") { if (cur.date.empty()) cur.date = words(rest).empty() ? "" : words(rest)[0]; }
            else if (code == "FT") feature_line(cur, line);
            else if (code == "SQ") in_seq = true;
            continue;
        }
        // GenBank
        if (line.rfind("LOCUS", 0) == 0) {
            if (in_rec) finish();
            in_rec = true;
            const auto w = words(line.substr(5));
            if (!w.empty()) cur.locus = w[0];
            for (size_t k = 1; k < w.size(); ++k) {
                if ((w[k] == "bp" || w[k] == "aa") && k >= 1) {
                    cur.length = std::strtoll(w[k - 1].c_str(), nullptr, 10);
                    size_t j = k + 1;
                    if (j < w.size() && w[j] != "linear" && w[j] != "circular") cur.molecule = w[j++];
                    if (j < w.size() && (w[j] == "linear" || w[j] == "circular")) cur.topology = w[j++];
                    if (j < w.size() && w[j].find('-') == std::string::npos) cur.division = w[j++];
                    if (j < w.size()) cur.date = w[j];
                    break;
                }
            }
            continue;
        }
        if (!in_rec) continue;
        if (in_seq) { add_sequence(cur.sequence, line); continue; }
        if (!line.empty() && line[0] != ' ') {
            const auto sp = line.find(' ');
            field = line.substr(0, sp);
            in_feat = field == "FEATURES";
            const std::string rest = line.size() > 12 ? trim(line.substr(12)) : "";
            if (field == "DEFINITION") cur.definition = rest;
            else if (field == "ACCESSION") cur.accession = words(rest).empty() ? "" : words(rest)[0];
            else if (field == "VERSION") cur.version = words(rest).empty() ? "" : words(rest)[0];
            else if (field == "KEYWORDS") cur.keywords = rest;
            else if (field == "ORIGIN") in_seq = true;
            continue;
        }
        if (in_feat) { feature_line(cur, line); continue; }
        // A continuation or a sub-field of the header.
        if (line.rfind("  ORGANISM", 0) == 0) {
            field = "ORGANISM";
            cur.organism = line.size() > 12 ? trim(line.substr(12)) : "";
            continue;
        }
        if (line.size() > 2 && line[2] != ' ') { field = trim(line.substr(0, 12)); continue; }
        const std::string t = trim(line);
        if (field == "DEFINITION") append(cur.definition, t);
        else if (field == "KEYWORDS") append(cur.keywords, t);
        else if (field == "ORGANISM") append(cur.taxonomy, t);
    }
    if (in_rec) finish();
    if (out->empty())
        return "'" + path + "': no " + std::string(embl ? "EMBL (ID line)" : "GenBank (LOCUS line)") +
               " record found";
    for (Record& r : *out) {
        r.definition = strip_final_dot(r.definition);
        r.keywords = r.keywords == "." ? "" : strip_final_dot(r.keywords);
        r.taxonomy = strip_final_dot(r.taxonomy);
        if (r.length < 0 && !r.sequence.empty()) r.length = (int64_t)r.sequence.size();
    }
    (void)line_no;
    return "";
}

std::shared_ptr<arrow::Array> str_col(const std::vector<std::optional<std::string>>& v) {
    arrow::StringBuilder b;
    for (const auto& x : v) ARROW_UNUSED(x ? b.Append(*x) : b.AppendNull());
    std::shared_ptr<arrow::Array> a;
    ARROW_UNUSED(b.Finish(&a));
    return a;
}

std::shared_ptr<arrow::Array> int_col(const std::vector<int64_t>& v) {
    arrow::Int64Builder b;
    for (int64_t x : v) ARROW_UNUSED(x < 0 ? b.AppendNull() : b.Append(x));
    std::shared_ptr<arrow::Array> a;
    ARROW_UNUSED(b.Finish(&a));
    return a;
}

std::optional<std::string> opt(const std::string& s) {
    return s.empty() ? std::nullopt : std::optional<std::string>(s);
}

struct Tables { std::shared_ptr<arrow::Table> features, records, sequences; int64_t n_features = 0; };

Tables build(const std::vector<Record>& recs, const std::vector<Region>& windows) {
    Tables t;
    // features: fixed columns, then one per qualifier key in order of first use.
    static const std::set<std::string> fixed{"record", "type", "start", "end", "strand", "location"};
    std::vector<std::string> keys;
    std::unordered_map<std::string, size_t> key_col;
    for (const Record& r : recs)
        for (const Feature& f : r.features)
            for (const auto& [k, v] : f.quals)
                if (!key_col.count(k)) { key_col[k] = keys.size(); keys.push_back(k); }
    std::vector<std::optional<std::string>> rec, type, strand, loc;
    std::vector<int64_t> start, end;
    std::vector<std::vector<std::optional<std::string>>> q(keys.size());
    for (const Record& r : recs) {
        for (const Feature& f : r.features) {
            int64_t s, e;
            char st;
            location_span(f.location, &s, &e, &st);
            if (!windows.empty()) {
                bool keep = false;
                for (const Region& w : windows)
                    keep = keep || ((w.chrom == r.id() || w.chrom == r.accession || w.chrom == r.locus) &&
                                    s >= 0 && s < w.end && e > w.start);
                if (!keep) continue;
            }
            rec.push_back(r.id());
            type.push_back(f.type);
            start.push_back(s);
            end.push_back(e);
            strand.push_back(std::string(1, st));
            loc.push_back(f.location);
            for (auto& col : q) col.emplace_back();
            for (const auto& [k, v] : f.quals) {
                auto& cell = q[key_col[k]].back();
                const std::string val = qual_value(v);
                if (cell) *cell += "; " + val;           // a repeated qualifier (db_xref)
                else cell = val;
            }
        }
    }
    t.n_features = (int64_t)rec.size();
    arrow::FieldVector ff{arrow::field("record", arrow::utf8()), arrow::field("type", arrow::utf8()),
                          arrow::field("start", arrow::int64()), arrow::field("end", arrow::int64()),
                          arrow::field("strand", arrow::utf8()), arrow::field("location", arrow::utf8())};
    arrow::ArrayVector fa{str_col(rec), str_col(type), int_col(start), int_col(end), str_col(strand),
                          str_col(loc)};
    for (size_t k = 0; k < keys.size(); ++k) {
        ff.push_back(arrow::field(fixed.count(keys[k]) ? "q_" + keys[k] : keys[k], arrow::utf8()));
        fa.push_back(str_col(q[k]));
    }
    t.features = arrow::Table::Make(arrow::schema(ff), fa, t.n_features);

    std::vector<std::optional<std::string>> id, locus, mol, topo, div, date, def, org, tax, kw, seq;
    std::vector<int64_t> len, nfeat, slen;
    bool any_seq = false;
    for (const Record& r : recs) {
        id.push_back(r.id());
        locus.push_back(opt(r.locus));
        len.push_back(r.length);
        mol.push_back(opt(r.molecule));
        topo.push_back(opt(r.topology));
        div.push_back(opt(r.division));
        date.push_back(opt(r.date));
        def.push_back(opt(r.definition));
        org.push_back(opt(r.organism));
        tax.push_back(opt(r.taxonomy));
        kw.push_back(opt(r.keywords));
        nfeat.push_back((int64_t)r.features.size());
        seq.push_back(opt(r.sequence));
        slen.push_back((int64_t)r.sequence.size());
        any_seq = any_seq || !r.sequence.empty();
    }
    t.records = arrow::Table::Make(
        arrow::schema({arrow::field("record", arrow::utf8()), arrow::field("locus", arrow::utf8()),
                       arrow::field("length", arrow::int64()), arrow::field("molecule", arrow::utf8()),
                       arrow::field("topology", arrow::utf8()), arrow::field("division", arrow::utf8()),
                       arrow::field("date", arrow::utf8()), arrow::field("definition", arrow::utf8()),
                       arrow::field("organism", arrow::utf8()), arrow::field("taxonomy", arrow::utf8()),
                       arrow::field("keywords", arrow::utf8()), arrow::field("features", arrow::int64())}),
        {str_col(id), str_col(locus), int_col(len), str_col(mol), str_col(topo), str_col(div), str_col(date),
         str_col(def), str_col(org), str_col(tax), str_col(kw), int_col(nfeat)});
    if (any_seq)
        t.sequences = arrow::Table::Make(
            arrow::schema({arrow::field("record", arrow::utf8()), arrow::field("length", arrow::int64()),
                           arrow::field("sequence", arrow::utf8())}),
            {str_col(id), int_col(slen), str_col(seq)});
    return t;
}

struct Tab { std::shared_ptr<arrow::Table> table; std::string label, footer; };

class FlatFileSource : public WorkbookSource {
public:
    FlatFileSource(std::shared_ptr<const std::vector<Tab>> tabs, size_t idx, std::string path, bool region)
        : WorkbookSource((*tabs)[idx].table, std::move(path), (*tabs)[idx].footer), tabs_(std::move(tabs)),
          idx_(idx), region_(region) {}
    std::string tab_label() const override { return (*tabs_)[idx_].label; }
    bool region_applied() const override { return region_; }
    std::vector<std::unique_ptr<TabularSource>> open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> v;
        if (idx_ != 0) return v;
        for (size_t k = 1; k < tabs_->size(); ++k)
            v.push_back(std::make_unique<FlatFileSource>(tabs_, k, path(), false));
        return v;
    }
private:
    std::shared_ptr<const std::vector<Tab>> tabs_;
    size_t idx_;
    bool   region_;
};

}  // namespace

std::string open_genbank_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    const std::string base = strip_compression_suffix(path);
    const bool embl = fends_ci(base, ".embl");
    std::vector<Record> recs;
    if (auto e = parse(path, embl, &recs); !e.empty()) return e;
    std::vector<Region> windows;
    if (!cfg.region.empty()) {
        windows = parse_region_list(cfg.region);
        resolve_region_chroms(windows, [&](const std::string& n) {
            for (const Record& r : recs)
                if (n == r.id() || n == r.accession || n == r.locus) return true;
            return false;
        }, path);
    }
    Tables t = build(recs, windows);
    const std::string fmt = embl ? "EMBL" : "GenBank";
    const std::string counts = std::to_string(recs.size()) + (recs.size() == 1 ? " record" : " records");
    auto tabs = std::make_shared<std::vector<Tab>>();
    tabs->push_back({t.features, "features",
                     "Format: " + fmt + " features  |  " + counts + "  |  start / end 0-based half-open; "
                     "location as in the file"});
    tabs->push_back({t.records, "records", "Format: " + fmt + " records  |  " + counts});
    if (t.sequences) tabs->push_back({t.sequences, "sequences", "Format: " + fmt + " sequences"});
    *out = std::make_unique<FlatFileSource>(tabs, 0, path, !cfg.region.empty());
    return "";
}
