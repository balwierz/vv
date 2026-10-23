// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Multiple sequence alignments: Stockholm (.sto .stk, Pfam / Rfam), Clustal
// (.aln), PHYLIP (.phy, sequential or interleaved, strict or relaxed names),
// NEXUS (.nex, the DATA / CHARACTERS matrix), MSF (.msf, GCG), A2M / A3M
// (.a2m .a3m, HMMER / HH-suite) and aligned FASTA (.afa). The file is read
// into memory as one row per sequence: its name, the description, the start
// and end a "name/12-345" suffix gives, its ungapped length, its gaps, its
// identity to the first sequence and the aligned sequence. Column
// annotations (Stockholm #=GC / #=GR, Clustal's conservation line) are rows
// too, under the sequences they annotate, so that they line up in a viewer.
// Stockholm's #=GF / #=GS lines make a second tab, "annotations".

#include "internal.hpp"

#include <cctype>
#include <map>

namespace {

struct Row {
    std::string name, desc, seq;
    bool        annotation = false;   // a column annotation, not a sequence
    int64_t     inserts = -1;         // A2M / A3M: insert-state residues removed
    int64_t     block = 1;            // Stockholm: the alignment in the file
};

struct Msa {
    std::string format;               // the format row's name
    std::vector<Row> rows;
    // Stockholm #=GF (name empty) and #=GS lines.
    struct Ann { std::string name, tag, value; };
    std::vector<Ann> ann;
    int64_t     blocks = 1;
    bool        a3m = false;          // an "inserts" column
};

bool is_gap(char c) { return c == '-' || c == '.' || c == '~'; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool starts_ci(const std::string& s, const char* prefix) {
    size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)prefix[i])) return false;
    return true;
}

// The non-blank characters of `s` from `from` on.
std::string residues(const std::string& s, size_t from = 0) {
    std::string r;
    for (size_t i = from; i < s.size(); ++i)
        if (!std::isspace((unsigned char)s[i])) r += s[i];
    return r;
}

// "name rest": the first whitespace-separated token and where the rest starts.
std::string first_token(const std::string& s, size_t* rest) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) { *rest = s.size(); return {}; }
    size_t b = s.find_first_of(" \t", a);
    if (b == std::string::npos) b = s.size();
    *rest = b;
    return s.substr(a, b - a);
}

using Lines = std::vector<std::string>;

// ── Stockholm ────────────────────────────────────────────────────────────────
// Sequence lines "name seq" (blocks of them, interleaved), #=GF tag text
// (file), #=GS name tag text (sequence), #=GR name tag line (residues),
// #=GC tag line (columns), "//" ending an alignment; a file may hold several.
std::string parse_stockholm(const Lines& lines, Msa* m) {
    std::vector<std::string> order;
    std::map<std::string, std::string> seq;
    std::vector<std::pair<std::string, std::string>> grOrder;      // (name, tag)
    std::map<std::pair<std::string, std::string>, std::string> gr;
    std::vector<std::string> gcOrder;
    std::map<std::string, std::string> gc;
    int64_t block = 1;
    bool any = false;
    auto flush = [&] {
        if (!any) return;
        for (const std::string& n : order) {
            m->rows.push_back({n, {}, seq[n], false, -1, block});
            for (const auto& key : grOrder)
                if (key.first == n)
                    m->rows.push_back({n + " #=GR " + key.second, "residue annotation", gr[key], true, -1, block});
        }
        for (const std::string& t : gcOrder)
            m->rows.push_back({"#=GC " + t, "column annotation", gc[t], true, -1, block});
        order.clear(); seq.clear(); grOrder.clear(); gr.clear(); gcOrder.clear(); gc.clear();
        any = false;
        ++block;
    };
    for (const std::string& raw : lines) {
        const std::string line = trim(raw);
        if (line.empty() || starts_ci(line, "# STOCKHOLM")) continue;
        if (line == "//") { flush(); continue; }
        size_t rest = 0;
        if (line.rfind("#=GF", 0) == 0) {
            first_token(line, &rest);
            size_t r2 = 0;
            const std::string tag = first_token(line.substr(rest), &r2);
            m->ann.push_back({{}, tag, trim(line.substr(rest + r2))});
        } else if (line.rfind("#=GS", 0) == 0) {
            first_token(line, &rest);
            std::string tail = line.substr(rest);
            size_t r2 = 0, r3 = 0;
            const std::string name = first_token(tail, &r2);
            const std::string tag = first_token(tail.substr(r2), &r3);
            m->ann.push_back({name, tag, trim(tail.substr(r2 + r3))});
        } else if (line.rfind("#=GR", 0) == 0) {
            first_token(line, &rest);
            std::string tail = line.substr(rest);
            size_t r2 = 0, r3 = 0;
            const std::string name = first_token(tail, &r2);
            const std::string tag = first_token(tail.substr(r2), &r3);
            auto key = std::make_pair(name, tag);
            if (!gr.count(key)) grOrder.push_back(key);
            gr[key] += residues(tail, r2 + r3);
            any = true;
        } else if (line.rfind("#=GC", 0) == 0) {
            first_token(line, &rest);
            size_t r2 = 0;
            std::string tail = line.substr(rest);
            const std::string tag = first_token(tail, &r2);
            if (!gc.count(tag)) gcOrder.push_back(tag);
            gc[tag] += residues(tail, r2);
            any = true;
        } else if (line[0] == '#') {
            continue;
        } else {
            const std::string name = first_token(line, &rest);
            if (!seq.count(name)) order.push_back(name);
            seq[name] += residues(line, rest);
            any = true;
        }
    }
    flush();
    m->blocks = block - 1;
    if (m->rows.empty()) return "no sequences in this Stockholm file";
    return "";
}

// ── Clustal ──────────────────────────────────────────────────────────────────
// A header line (CLUSTAL W / Clustal Omega / MUSCLE), then blocks of
// "name seq [count]" lines, each followed by the conservation line: '*', ':'
// and '.' under the block's columns (blank where nothing is conserved, which
// some writers save as an empty line).
std::string parse_clustal(const Lines& lines, Msa* m) {
    std::vector<std::string> order;
    std::map<std::string, std::string> seq;
    std::string cons;
    size_t offset = std::string::npos, width = 0, blockStart = 0;
    bool header = true;
    for (const std::string& raw : lines) {
        std::string line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (header) { header = false; continue; }   // CLUSTAL W (1.83) multiple sequence alignment
        if (line.empty()) continue;
        if (std::isspace((unsigned char)line[0])) {
            if (offset == std::string::npos) continue;
            cons.resize(blockStart, ' ');
            std::string part = offset < line.size() ? line.substr(offset, width) : std::string();
            part.resize(width, ' ');
            cons += part;
            continue;
        }
        size_t rest = 0;
        const std::string name = first_token(line, &rest);
        size_t at = line.find_first_not_of(" \t", rest);
        if (at == std::string::npos) continue;
        size_t end = line.find_first_of(" \t", at);
        const std::string chunk = line.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!seq.count(name)) order.push_back(name);
        if (!order.empty() && name == order.front()) {   // a block begins
            blockStart = seq[name].size();
            offset = at;
            width = chunk.size();
        }
        seq[name] += chunk;
    }
    if (order.empty()) return "no sequences in this Clustal file";
    for (const std::string& n : order) m->rows.push_back({n, {}, seq[n]});
    cons.resize(seq[order.front()].size(), ' ');
    if (cons.find_first_not_of(' ') != std::string::npos)
        m->rows.push_back({"conservation", "column annotation", cons, true});
    return "";
}

// ── PHYLIP ───────────────────────────────────────────────────────────────────
// "ntax nchar", then the sequences: sequential (each whole, perhaps over
// several lines) or interleaved (names on the first block only), with strict
// 10-character names or relaxed ones ended by a blank. Each reading is tried
// and the first that gives every sequence nchar characters is taken.
std::string parse_phylip(const Lines& lines, Msa* m) {
    Lines body;
    int64_t ntax = -1, nchar = -1;
    for (const std::string& raw : lines) {
        const std::string line = trim(raw);
        if (ntax < 0) {
            if (line.empty()) continue;
            std::istringstream in(line);
            if (!(in >> ntax >> nchar) || ntax <= 0 || nchar < 0)
                return "a PHYLIP file starts with the number of sequences and of characters";
            continue;
        }
        if (!line.empty()) body.push_back(raw);
    }
    if (ntax < 0) return "empty PHYLIP file";
    if ((int64_t)body.size() < ntax) return "the PHYLIP file has fewer sequences than its header says";
    auto name_of = [](const std::string& line, bool strict, size_t* rest) {
        if (strict) {
            *rest = std::min<size_t>(10, line.size());
            return trim(line.substr(0, *rest));
        }
        return first_token(line, rest);
    };
    for (int attempt = 0; attempt < 4; ++attempt) {
        const bool strict = attempt >= 2, interleaved = attempt % 2 == 0;
        std::vector<Row> rows;
        size_t li = 0;
        bool ok = true;
        if (interleaved) {
            for (int64_t i = 0; i < ntax; ++i, ++li) {
                size_t rest = 0;
                std::string name = name_of(body[li], strict, &rest);
                rows.push_back({name, {}, residues(body[li], rest)});
            }
            for (int64_t k = 0; li < body.size(); ++li, ++k) rows[(size_t)(k % ntax)].seq += residues(body[li]);
        } else {
            for (int64_t i = 0; i < ntax && ok; ++i) {
                if (li >= body.size()) { ok = false; break; }
                size_t rest = 0;
                std::string name = name_of(body[li], strict, &rest);
                Row r{name, {}, residues(body[li], rest)};
                ++li;
                while ((int64_t)r.seq.size() < nchar && li < body.size()) r.seq += residues(body[li++]);
                rows.push_back(std::move(r));
            }
            ok = ok && li == body.size();
        }
        for (const Row& r : rows) ok = ok && (int64_t)r.seq.size() == nchar;
        if (ok || attempt == 3) {
            if (!ok) {   // none fits: the relaxed interleaved reading, as is
                rows.clear();
                li = 0;
                for (int64_t i = 0; i < ntax; ++i, ++li) {
                    size_t rest = 0;
                    std::string name = first_token(body[li], &rest);
                    rows.push_back({name, {}, residues(body[li], rest)});
                }
                for (int64_t k = 0; li < body.size(); ++li, ++k) rows[(size_t)(k % ntax)].seq += residues(body[li]);
            }
            m->rows = std::move(rows);
            return "";
        }
    }
    return "";
}

// ── NEXUS ────────────────────────────────────────────────────────────────────
// The MATRIX of the DATA or CHARACTERS block: "name seq" lines, names maybe
// quoted, interleaved blocks repeating the names; FORMAT MATCHCHAR=. is
// spelled out against the first sequence. Comments in [ ] are dropped.
std::string parse_nexus(const Lines& lines, Msa* m) {
    std::string text;
    int depth = 0;
    for (const std::string& line : lines) {
        for (char c : line) {
            if (c == '[') ++depth;
            else if (c == ']' && depth > 0) --depth;
            else if (depth == 0) text += c;
        }
        text += '\n';
    }
    std::string low = text;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    size_t blk = std::string::npos;
    for (const char* b : {"begin data;", "begin characters;"})
        if (size_t p = low.find(b); p != std::string::npos && p < blk) blk = p;
    if (blk == std::string::npos) return "no DATA or CHARACTERS block in this NEXUS file";
    const size_t endBlk = low.find("end;", blk);
    char match = 0;
    int64_t nchar = -1;
    if (size_t f = low.find("matchchar", blk); f != std::string::npos && f < endBlk) {
        size_t eq = low.find('=', f);
        if (eq != std::string::npos) {
            size_t c = low.find_first_not_of(" \t", eq + 1);
            if (c != std::string::npos) match = text[c];
        }
    }
    if (size_t f = low.find("nchar", blk); f != std::string::npos && f < endBlk) {
        size_t eq = low.find('=', f);
        if (eq != std::string::npos) nchar = std::atoll(low.c_str() + eq + 1);
    }
    size_t mat = low.find("matrix", blk);
    if (mat == std::string::npos || (endBlk != std::string::npos && mat > endBlk))
        return "no MATRIX in this NEXUS file";
    size_t stop = text.find(';', mat);
    std::istringstream body(text.substr(mat + 6, stop == std::string::npos ? std::string::npos : stop - mat - 6));
    std::vector<std::string> order;
    std::map<std::string, std::string> seq;
    for (std::string line; std::getline(body, line);) {
        const std::string t = trim(line);
        if (t.empty()) continue;
        std::string name;
        size_t rest = 0;
        if (t[0] == '\'') {
            size_t i = 1;
            for (; i < t.size(); ++i) {
                if (t[i] == '\'' && i + 1 < t.size() && t[i + 1] == '\'') { name += '\''; ++i; }
                else if (t[i] == '\'') break;
                else name += t[i];
            }
            rest = i + 1;
        } else {
            name = first_token(t, &rest);
        }
        // A line that does not name a sequence continues the last one
        // (a sequential matrix wrapped over several lines).
        if (!seq.count(name) && !order.empty() && nchar > 0 &&
            (int64_t)seq[order.back()].size() < nchar && name.size() == t.size()) {
            seq[order.back()] += residues(t);
            continue;
        }
        if (!seq.count(name)) order.push_back(name);
        seq[name] += residues(t, rest);
    }
    if (order.empty()) return "an empty NEXUS MATRIX";
    const std::string& first = seq[order.front()];
    for (const std::string& n : order) {
        std::string s = seq[n];
        if (match && n != order.front())
            for (size_t i = 0; i < s.size() && i < first.size(); ++i)
                if (s[i] == match) s[i] = first[i];
        m->rows.push_back({n, {}, s});
    }
    return "";
}

// ── MSF (GCG) ────────────────────────────────────────────────────────────────
// A header with one "Name: x Len: n Check: c Weight: w" line per sequence,
// "//", then blocks of "name seq seq seq …" (in groups of ten) under lines
// of column numbers.
std::string parse_msf(const Lines& lines, Msa* m) {
    std::vector<std::string> order;
    std::map<std::string, std::string> seq;
    bool body = false;
    for (const std::string& raw : lines) {
        const std::string line = trim(raw);
        if (!body) {
            if (line.rfind("//", 0) == 0) { body = true; continue; }
            if (size_t p = line.find("Name:"); p != std::string::npos) {
                size_t rest = 0;
                std::string name = first_token(line.substr(p + 5), &rest);
                if (!name.empty() && !seq.count(name)) { order.push_back(name); seq[name]; }
            }
            continue;
        }
        if (line.empty()) continue;
        size_t rest = 0;
        const std::string name = first_token(line, &rest);
        if (seq.count(name)) seq[name] += residues(line, rest);
    }
    if (!body) return "no \"//\" ending the MSF header";
    if (order.empty()) return "no Name: lines in this MSF file";
    for (const std::string& n : order) {
        std::string s = seq[n];
        for (char& c : s) if (c == '.' || c == '~') c = '-';   // MSF's gaps, as the other formats write them
        m->rows.push_back({n, {}, s});
    }
    return "";
}

// ── A2M / A3M and aligned FASTA ──────────────────────────────────────────────
// FASTA records; in A2M / A3M upper case and '-' are match states and lower
// case (and A2M's '.') insert states, which are left out of the aligned
// sequence and counted in "inserts".
std::string parse_fasta(const Lines& lines, bool a3m, Msa* m) {
    for (const std::string& raw : lines) {
        if (raw.empty()) continue;
        if (raw[0] == '#') continue;                         // A3M's "#A3M#" and the like
        if (raw[0] == '>') {
            size_t rest = 0;
            const std::string h = trim(raw.substr(1));
            std::string name = first_token(h, &rest);
            m->rows.push_back({name, trim(h.substr(rest)), {}, false, a3m ? 0 : -1});
            continue;
        }
        if (m->rows.empty()) return "a FASTA alignment starts with a '>' line";
        Row& r = m->rows.back();
        for (char c : raw) {
            if (std::isspace((unsigned char)c)) continue;
            if (a3m && (std::islower((unsigned char)c) || c == '.')) {
                if (c != '.') ++r.inserts;
                continue;
            }
            r.seq += c;
        }
    }
    if (m->rows.empty()) return "no sequences in this alignment";
    m->a3m = a3m;
    return "";
}

std::string parse(const std::string& kind, const Lines& lines, Msa* m) {
    if (kind == "stockholm") { m->format = "Stockholm";     return parse_stockholm(lines, m); }
    if (kind == "clustal")   { m->format = "Clustal";       return parse_clustal(lines, m); }
    if (kind == "phylip")    { m->format = "PHYLIP";        return parse_phylip(lines, m); }
    if (kind == "nexus")     { m->format = "NEXUS";         return parse_nexus(lines, m); }
    if (kind == "msf")       { m->format = "MSF";           return parse_msf(lines, m); }
    if (kind == "a3m")       { m->format = "A2M / A3M";     return parse_fasta(lines, true, m); }
    m->format = "Aligned FASTA";
    return parse_fasta(lines, false, m);
}

// ── The tables ───────────────────────────────────────────────────────────────

// Strings go through valid_utf8: a file's bytes need not be UTF-8 (a Latin-1
// description), an Arrow string column must be.
std::shared_ptr<arrow::Array> finish(arrow::ArrayBuilder& b) {
    std::shared_ptr<arrow::Array> a;
    ARROW_UNUSED(b.Finish(&a));
    return a;
}

struct Built {
    std::shared_ptr<arrow::Table> alignment, annotations;
    std::string footer, alphabet;
};

Built build(const Msa& m) {
    Built out;
    arrow::Int64Builder blockB, startB, endB, lenB, gapB, insB;
    arrow::StringBuilder nameB, descB, seqB;
    arrow::DoubleBuilder idB;
    // Residues of the sequences (not annotations): nucleotide or protein,
    // the --seq-stats rule with gaps left out.
    int64_t total = 0, nuc = 0, nseq = 0, width = 0;
    bool unequal = false;
    std::map<int64_t, const Row*> firstOf;   // each alignment's first sequence
    for (const Row& r : m.rows) {
        if (r.annotation) continue;
        ++nseq;
        if (!firstOf.count(r.block)) firstOf[r.block] = &r;
        if (width && (int64_t)r.seq.size() != width) unequal = true;
        width = std::max<int64_t>(width, (int64_t)r.seq.size());
        for (char c : r.seq) {
            if (is_gap(c)) continue;
            ++total;
            if (std::strchr("ACGTUNacgtun", c)) ++nuc;
        }
    }
    out.alphabet = total == 0 ? "" : (total - nuc) > total / 10 ? "protein" : "nucleotide";
    for (const Row& r : m.rows) {
        if (m.blocks > 1) ARROW_UNUSED(blockB.Append(r.block));
        ARROW_UNUSED(nameB.Append(valid_utf8(r.name)));
        ARROW_UNUSED(r.desc.empty() ? descB.AppendNull() : descB.Append(valid_utf8(r.desc)));
        int64_t start = -1, end = -1;
        if (!r.annotation) {
            // "name/12-345": the residues the sequence covers (1-based, inclusive).
            size_t slash = r.name.rfind('/');
            size_t dash = slash == std::string::npos ? std::string::npos : r.name.find('-', slash);
            if (dash != std::string::npos && dash > slash + 1 && dash + 1 < r.name.size() &&
                r.name.find_first_not_of("0123456789", slash + 1) == dash &&
                r.name.find_first_not_of("0123456789", dash + 1) == std::string::npos) {
                start = std::atoll(r.name.c_str() + slash + 1);
                end = std::atoll(r.name.c_str() + dash + 1);
            }
        }
        ARROW_UNUSED(start < 0 ? startB.AppendNull() : startB.Append(start));
        ARROW_UNUSED(end < 0 ? endB.AppendNull() : endB.Append(end));
        if (r.annotation) {
            ARROW_UNUSED(lenB.AppendNull());
            ARROW_UNUSED(gapB.AppendNull());
            ARROW_UNUSED(idB.AppendNull());
        } else {
            int64_t gaps = 0;
            for (char c : r.seq) gaps += is_gap(c);
            ARROW_UNUSED(lenB.Append((int64_t)r.seq.size() - gaps + std::max<int64_t>(r.inserts, 0)));
            ARROW_UNUSED(gapB.Append(gaps));
            // Identity to the alignment's first sequence, over the columns
            // where neither has a gap.
            const std::string& ref = firstOf[r.block]->seq;
            int64_t same = 0, both = 0;
            for (size_t i = 0; i < r.seq.size() && i < ref.size(); ++i) {
                if (is_gap(r.seq[i]) || is_gap(ref[i])) continue;
                ++both;
                same += std::toupper((unsigned char)r.seq[i]) == std::toupper((unsigned char)ref[i]);
            }
            ARROW_UNUSED(both ? idB.Append(std::round(1000.0 * same / both) / 10) : idB.AppendNull());
        }
        if (m.a3m) ARROW_UNUSED(r.annotation ? insB.AppendNull() : insB.Append(r.inserts));
        ARROW_UNUSED(seqB.Append(valid_utf8(r.seq)));
    }
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> cols;
    if (m.blocks > 1) { fields.push_back(arrow::field("alignment", arrow::int64())); cols.push_back(finish(blockB)); }
    for (auto [name, type, b] : std::initializer_list<std::tuple<const char*, std::shared_ptr<arrow::DataType>,
                                                                 arrow::ArrayBuilder*>>{
             {"name", arrow::utf8(), &nameB}, {"description", arrow::utf8(), &descB},
             {"start", arrow::int64(), &startB}, {"end", arrow::int64(), &endB},
             {"length", arrow::int64(), &lenB}, {"gaps", arrow::int64(), &gapB},
             {"identity", arrow::float64(), &idB}}) {
        fields.push_back(arrow::field(name, type));
        cols.push_back(finish(*b));
    }
    if (m.a3m) { fields.push_back(arrow::field("inserts", arrow::int64())); cols.push_back(finish(insB)); }
    fields.push_back(arrow::field("sequence", arrow::utf8()));
    cols.push_back(finish(seqB));
    out.alignment = arrow::Table::Make(arrow::schema(fields), cols);

    if (!m.ann.empty()) {
        arrow::StringBuilder an, at, av;
        for (const Msa::Ann& a : m.ann) {
            ARROW_UNUSED(a.name.empty() ? an.AppendNull() : an.Append(valid_utf8(a.name)));
            ARROW_UNUSED(at.Append(valid_utf8(a.tag)));
            ARROW_UNUSED(av.Append(valid_utf8(a.value)));
        }
        out.annotations = arrow::Table::Make(
            arrow::schema({arrow::field("name", arrow::utf8()), arrow::field("tag", arrow::utf8()),
                           arrow::field("value", arrow::utf8())}),
            {finish(an), finish(at), finish(av)});
    }
    out.footer = "Format: " + m.format + "  |  " + digits_with_sep(std::to_string(nseq)) +
                 (nseq == 1 ? " sequence" : " sequences") + " × " +
                 digits_with_sep(std::to_string(width)) + " columns";
    if (!out.alphabet.empty()) out.footer += "  |  " + out.alphabet;
    if (m.blocks > 1) out.footer += "  |  " + std::to_string(m.blocks) + " alignments";
    if (unequal) out.footer += "  |  sequences of unequal length";
    if (m.blocks <= 1 && !unequal) out.footer += "  |  start / end from name/start-end, 1-based";
    return out;
}

struct Tab { std::shared_ptr<arrow::Table> table; std::string label, footer; };

class MsaSource : public WorkbookSource {
public:
    MsaSource(std::shared_ptr<const std::vector<Tab>> tabs, size_t idx, std::string path, std::string alphabet)
        : WorkbookSource((*tabs)[idx].table, std::move(path), (*tabs)[idx].footer), tabs_(std::move(tabs)),
          idx_(idx), alphabet_(std::move(alphabet)) {}
    std::string tab_label() const override { return (*tabs_)[idx_].label; }
    std::string sequence_alphabet() const override { return idx_ == 0 ? alphabet_ : std::string(); }
    std::vector<std::unique_ptr<TabularSource>> open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> v;
        if (idx_ != 0) return v;
        for (size_t k = 1; k < tabs_->size(); ++k)
            v.push_back(std::make_unique<MsaSource>(tabs_, k, path(), alphabet_));
        return v;
    }
private:
    std::shared_ptr<const std::vector<Tab>> tabs_;
    size_t      idx_;
    std::string alphabet_;
};

std::string read_lines(std::shared_ptr<arrow::io::InputStream> in, Lines* out) {
    LineReader lr(std::move(in));
    std::string line;
    while (true) {
        const bool nl = lr.read_line(&line);
        if (!nl && line.empty()) break;
        out->push_back(line);
        if (!nl) break;
    }
    return "";
}

std::string kind_of_head(const std::string& head, const std::string& path) {
    std::istringstream in(head);
    std::string first;
    while (std::getline(in, first))
        if (!trim(first).empty()) break;
    first = trim(first);
    if (starts_ci(first, "# STOCKHOLM")) return "stockholm";
    if (starts_ci(first, "CLUSTAL") || starts_ci(first, "MUSCLE (") || starts_ci(first, "PROBCONS"))
        return "clustal";
    if (starts_ci(first, "#NEXUS")) return "nexus";
    if (starts_ci(first, "!!AA_MULTIPLE_ALIGNMENT") || starts_ci(first, "!!NA_MULTIPLE_ALIGNMENT") ||
        starts_ci(first, "PileUp") || (head.find(" MSF:") != std::string::npos &&
                                       head.find("Check:") != std::string::npos &&
                                       head.find("//") != std::string::npos))
        return "msf";
    {
        std::istringstream l(first);
        int64_t a = -1, b = -1;
        std::string more;
        if (l >> a >> b && a > 0 && b >= 0 && (!(l >> more) || more.find_first_not_of("SsIi") == std::string::npos))
            return "phylip";
    }
    if (first[0] == '>' || starts_ci(first, "#A3M")) {
        const std::string base = strip_compression_suffix(path);
        return fends_ci(base, ".a2m") || fends_ci(base, ".a3m") || starts_ci(first, "#A3M") ? "a3m" : "afa";
    }
    return "";
}

}  // namespace

std::string msa_kind(const std::string& path) {
    std::shared_ptr<arrow::io::InputStream> in;
    if (!open_decoded_file(path, &in).empty()) return "";
    auto buf = in->Read(8192);
    if (!buf.ok()) return "";
    return kind_of_head((*buf)->ToString(), path);
}

bool fasta_is_aligned(const std::string& path) {
    std::shared_ptr<arrow::io::InputStream> in;
    if (!open_decoded_file(path, &in).empty()) return false;
    auto buf = in->Read(1 << 16);
    if (!buf.ok()) return false;
    const std::string head = (*buf)->ToString();
    const bool whole = head.size() < (1 << 16);
    std::vector<size_t> lens;
    bool gap = false;
    size_t cur = 0;
    bool in_rec = false;
    std::istringstream s(head);
    for (std::string line; std::getline(s, line);) {
        if (!line.empty() && line[0] == '>') {
            if (in_rec) lens.push_back(cur);
            in_rec = true;
            cur = 0;
            continue;
        }
        for (char c : line)
            if (!std::isspace((unsigned char)c)) { ++cur; gap = gap || c == '-'; }
    }
    if (in_rec && whole) lens.push_back(cur);   // the last record, when it ends in the sample
    if (lens.size() < 2 || !gap || lens.front() == 0) return false;
    for (size_t n : lens) if (n != lens.front()) return false;
    return true;
}

std::string open_msa_source(const std::string& path, const Config& /*cfg*/, std::unique_ptr<TabularSource>* out,
                            std::string kind) {
    if (kind.empty()) kind = msa_kind(path);
    if (kind.empty()) {   // by extension, for a file whose start says nothing
        static const std::pair<const char*, const char*> kByExt[] = {
            {".sto", "stockholm"}, {".stk", "stockholm"}, {".stockholm", "stockholm"},
            {".aln", "clustal"},   {".clustal", "clustal"}, {".clw", "clustal"}, {".clustalw", "clustal"},
            {".phy", "phylip"},    {".phylip", "phylip"},
            {".nex", "nexus"},     {".nxs", "nexus"},       {".nexus", "nexus"},
            {".msf", "msf"},       {".a2m", "a3m"},         {".a3m", "a3m"},
            {".afa", "afa"},       {".afasta", "afa"}};
        const std::string base = strip_compression_suffix(path);
        for (const auto& [ext, k] : kByExt)
            if (fends_ci(base, ext)) kind = k;
    }
    if (kind.empty()) return "'" + path + "': not a multiple sequence alignment vv reads";
    std::shared_ptr<arrow::io::InputStream> in;
    if (auto e = open_decoded_file(path, &in); !e.empty()) return e;
    Lines lines;
    read_lines(in, &lines);
    Msa m;
    if (auto e = parse(kind, lines, &m); !e.empty()) return e;
    Built b = build(m);
    auto tabs = std::make_shared<std::vector<Tab>>();
    tabs->push_back({b.alignment, "alignment", b.footer});
    if (b.annotations)
        tabs->push_back({b.annotations, "annotations",
                         "Format: " + m.format + " annotations  |  #=GF (file) and #=GS (sequence) lines"});
    *out = std::make_unique<MsaSource>(tabs, 0, path, b.alphabet);
    return "";
}

#ifdef VV_FUZZ
// Parse an untrusted buffer as each alignment format and build the tables of
// those that accept it; abort when a built table is not valid.
void msa_fuzz_one(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    Lines lines;
    std::istringstream s(text);
    for (std::string l; std::getline(s, l);) lines.push_back(l);
    for (const char* kind : {"stockholm", "clustal", "phylip", "nexus", "msf", "a3m", "afa"}) {
        Msa m;
        if (!parse(kind, lines, &m).empty()) continue;
        Built b = build(m);
        if (!b.alignment->ValidateFull().ok()) std::abort();
        if (b.annotations && !b.annotations->ValidateFull().ok()) std::abort();
    }
}
#endif
