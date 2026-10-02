// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Region strings, chromosome aliasing and htslib region conventions.

#include "internal.hpp"

// ── UCSC <-> Ensembl chromosome-name aliasing for region queries ─────────────
// A `-r chr1:…` query against a file that names the contig `1` (or vice versa)
// otherwise silently returns zero rows. The alias applies ONLY to human/mouse
// standard chromosomes — autosomes 1..22 (covers human 1-22 and mouse 1-19), X,
// Y, and the mitochondrion — so scaffolds / patches / alt-contigs are never
// remapped. The mitochondrion is `chrM` <-> `MT` (never `M`).
std::string chrom_alias(const std::string& n) {
    auto std_core = [](const std::string& c) -> bool {   // 1..22, X, Y
        if (c == "X" || c == "Y") return true;
        if (c.empty() || c.size() > 2) return false;
        for (char ch : c) if (ch < '0' || ch > '9') return false;
        int v = std::atoi(c.c_str());
        return v >= 1 && v <= 22;
    };
    if (n.size() > 3 && n.compare(0, 3, "chr") == 0) {    // UCSC -> Ensembl
        std::string core = n.substr(3);
        if (core == "M" || core == "MT") return "MT";      // chrM -> MT
        if (std_core(core)) return core;                   // chr1 -> 1, chrX -> X
        return "";
    }
    if (n == "MT" || n == "M") return "chrM";              // MT -> chrM
    if (std_core(n)) return "chr" + n;                     // 1 -> chr1, X -> chrX
    return "";
}


// Streams the lines emitted by a tabix iterator (one or more comma-separated
// regions over a tabix-indexed bgzipped file) as if they were the data portion
// of the original file. Used to feed Arrow's CSV/TSV reader with only the
// records that overlap a requested region.


// All four preamble strippers read through the buffered LineReader (8 KiB
// reads) rather than one byte at a time — a VCF with a 50–100 KiB ## header
// otherwise meant 100 K single-byte Read()s, each allocating an Arrow Buffer.
// When the first data line is reached it (plus any bytes the LineReader read
// ahead, via leftover()) is handed back through *put_back, which the caller
// replays with a PrependInputStream — so there is no need to Seek the
// underlying stream back, and the gz / stdin (non-seekable) and regular-file
// paths are now identical.

// Reads and strips "track"/"browser" preamble lines from the current position.
std::vector<std::string> strip_bed_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input, std::string* put_back)
{
    std::vector<std::string> headers;
    LineReader lr(input);
    for (;;) {
        std::string line;
        bool ok = lr.read_line(&line);
        if (!ok && line.empty()) break;
        auto has_prefix = [&](const char* p, size_t n) {
            return line.size() >= n && line.compare(0, n, p, n) == 0 &&
                   (line.size() == n || line[n] == ' ' || line[n] == '\t');
        };
        if (has_prefix("track", 5) || has_prefix("browser", 7)) {
            headers.push_back(line);
            if (!ok) break;
        } else {
            if (put_back) *put_back = line + "\n" + lr.leftover();
            break;
        }
    }
    return headers;
}

// Strips lines whose first character equals prefix_char (e.g. '#' for GFF3,
// '@' for SAM); the first data line (+ look-ahead) goes to *put_back.
std::vector<std::string> strip_prefix_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    char prefix_char, std::string* put_back)
{
    std::vector<std::string> preamble;
    LineReader lr(input);
    for (;;) {
        std::string line;
        bool ok = lr.read_line(&line);
        if (!ok && line.empty()) break;
        if (!line.empty() && line[0] == prefix_char) {
            preamble.push_back(line);
            if (!ok) break;
        } else {
            if (put_back) *put_back = line + "\n" + lr.leftover();
            break;
        }
    }
    return preamble;
}

// GCT (GenePattern): "#1.2" / "#1.3", a dimensions line (rows, columns[,
// row-metadata columns, column-metadata rows]), the header, then — in 1.3 —
// one row per column-metadata field before the data. The version, dimensions
// and metadata rows are returned as preamble; the header fills *col_names.
std::vector<std::string> strip_gct_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    std::string* put_back, std::vector<std::string>* col_names)
{
    std::vector<std::string> preamble;
    LineReader lr(input);
    std::string line;
    if (!lr.read_line(&line) && line.empty()) return preamble;
    preamble.push_back(line);                        // #1.2 / #1.3
    if (!lr.read_line(&line) && line.empty()) return preamble;
    preamble.push_back(line);                        // dimensions
    std::istringstream dims(line);
    long long nrow = 0, ncol = 0, nrowmeta = 0, ncolmeta = 0;
    dims >> nrow >> ncol >> nrowmeta >> ncolmeta;
    bool more = lr.read_line(&line);
    if (!more && line.empty()) return preamble;
    col_names->clear();                              // header
    for (size_t a = 0;;) {
        size_t b = line.find('\t', a);
        col_names->push_back(line.substr(a, b == std::string::npos ? b : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    for (long long k = 0; k < ncolmeta && more; ++k) {
        more = lr.read_line(&line);
        if (!line.empty()) preamble.push_back(line); // column-metadata row
    }
    *put_back = lr.leftover();
    return preamble;
}

// 4DN .pairs: "## pairs format v1.0", then "#" header lines, one of which is
// "#columns: readID chrom1 pos1 chrom2 pos2 strand1 strand2 …" naming the
// data columns (the first seven are mandatory; without the line, those seven
// names are used). The header lines are returned as preamble.
std::vector<std::string> strip_pairs_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    std::string* put_back, std::vector<std::string>* col_names)
{
    std::vector<std::string> preamble;
    LineReader lr(input);
    for (;;) {
        std::string line;
        bool ok = lr.read_line(&line);
        if (!ok && line.empty()) break;
        if (!line.empty() && line[0] == '#') {
            preamble.push_back(line);
            if (line.rfind("#columns:", 0) == 0) {
                col_names->clear();
                std::istringstream names(line.substr(9));
                for (std::string w; names >> w;) col_names->push_back(w);
            }
            if (!ok) break;
        } else {
            *put_back = line + "\n" + lr.leftover();
            break;
        }
    }
    if (col_names->empty())
        *col_names = {"readID", "chrom1", "pos1", "chrom2", "pos2", "strand1", "strand2"};
    return preamble;
}

// VCF: strips ## meta-information lines; reads the #CHROM header line for column names.
// The #CHROM line is consumed; stream is left at the first data line.
// *col_names_out is populated from #CHROM; returned vector contains ## lines only.
std::vector<std::string> strip_vcf_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    std::vector<std::string>* col_names_out, std::string* put_back)
{
    std::vector<std::string> preamble;
    LineReader lr(input);
    for (;;) {
        std::string line;
        bool ok = lr.read_line(&line);
        if (!ok && line.empty()) break;
        if (line.size() >= 2 && line[0] == '#' && line[1] == '#') {
            preamble.push_back(line);
            if (!ok) break;
        } else if (!line.empty() && line[0] == '#') {
            // #CHROM line: strip leading '#', split on tab → column names. The
            // data follows it; hand the LineReader's look-ahead back so the CSV
            // reader resumes exactly at the first record.
            std::istringstream ss(line.substr(1));
            std::string tok;
            while (std::getline(ss, tok, '\t'))
                col_names_out->push_back(tok);
            if (put_back) *put_back = lr.leftover();
            break;
        } else {
            // data before #CHROM (malformed): replay it rather than drop it.
            if (put_back) *put_back = line + "\n" + lr.leftover();
            break;
        }
    }
    return preamble;
}

// CSV/TSV: strips any leading '#'-prefixed lines as preamble. If the last such
// line has a single '#' (not '##') and its field count matches the first data
// line, treat it as the header row (returned via *col_names_out, stripped of
// the leading '#'); otherwise leave it in the preamble.
std::vector<std::string> strip_tsv_csv_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    char delim, std::string* put_back,
    std::vector<std::string>* col_names_out)
{
    std::vector<std::string> preamble;
    std::string first_data_line;
    LineReader lr(input);
    for (;;) {
        std::string line;
        bool ok = lr.read_line(&line);
        if (!ok && line.empty()) break;
        if (!line.empty() && line[0] == '#') {
            preamble.push_back(line);
            if (!ok) break;
        } else {
            first_data_line = line;
            if (put_back) *put_back = line + "\n" + lr.leftover();
            break;
        }
    }
    if (!preamble.empty() && !first_data_line.empty()) {
        const std::string& last = preamble.back();
        if (last.size() >= 2 && last[0] == '#' && last[1] != '#') {
            auto count_fields = [&](const std::string& s) {
                int n = 1;
                for (char c : s) if (c == delim) ++n;
                return n;
            };
            if (count_fields(last.substr(1)) == count_fields(first_data_line)) {
                std::istringstream ss(last.substr(1));
                std::string tok;
                while (std::getline(ss, tok, delim))
                    col_names_out->push_back(tok);
                preamble.pop_back();
            }
        }
    }
    return preamble;
}


// Parse "chrom[:start[-end]]" into a Region. Output is always normalized
// to the UCSC convention (0-based half-open). When one_based is true,
// the input is interpreted per the NCBI / GenBank / VCF / GFF / tabix /
// samtools convention (1-based inclusive at both ends) and converted
// internally before storage.
bool parse_region_one(const std::string& s, Region* out,
                       bool one_based) {
    auto colon = s.find(':');
    if (colon == std::string::npos) {
        out->chrom = s;
        out->start = INT64_MIN;
        out->end   = INT64_MAX;
        return !out->chrom.empty();
    }
    out->chrom = s.substr(0, colon);
    if (out->chrom.empty()) return false;
    std::string rest = s.substr(colon + 1);
    if (rest.empty()) {
        out->start = INT64_MIN;
        out->end   = INT64_MAX;
        return true;
    }
    auto dash = rest.find('-');
    std::string a, b;
    if (dash == std::string::npos) { a = rest; }
    else { a = rest.substr(0, dash); b = rest.substr(dash + 1); }
    auto parse_int = [](const std::string& t, int64_t* v) {
        if (t.empty()) return true;  // open end
        try {
            size_t pos = 0;
            *v = std::stoll(t, &pos);
            return pos == t.size();   // reject trailing garbage ("5x", "5-10")
        } catch (...) { return false; }
    };
    int64_t pa = INT64_MIN, pb = INT64_MAX;
    bool have_a = !a.empty();
    bool have_b = (dash != std::string::npos) && !b.empty();
    if (have_a && !parse_int(a, &pa)) return false;
    if (have_b && !parse_int(b, &pb)) return false;

    if (one_based) {
        // NCBI-style 1-based inclusive → UCSC 0-based half-open
        //   "a-b"   NCBI [a, b] inclusive  →  UCSC [a - 1, b)
        //   "a"     NCBI single position a →  UCSC [a - 1, a)
        //   "a-"    open upper             →  UCSC [a - 1, INT64_MAX)
        //   "-b"    open lower             →  UCSC [0, b)
        if (have_a) out->start = std::max<int64_t>(0, pa - 1);
        else        out->start = INT64_MIN;
        if (dash == std::string::npos) {
            out->end = have_a ? pa : INT64_MAX;   // single position
        } else {
            out->end = have_b ? pb : INT64_MAX;
        }
    } else {
        // BED-style coordinates: already 0-based half-open
        out->start = have_a ? pa : INT64_MIN;
        if (dash == std::string::npos) {
            // "chrom:N" → single 0-based position [N, N+1)
            out->end = have_a ? out->start + 1 : INT64_MAX;
        } else {
            out->end = have_b ? pb : INT64_MAX;
        }
    }
    return true;
}

std::vector<Region> parse_region_list(const std::string& spec,
                                       bool one_based) {
    std::vector<Region> out;
    size_t start = 0;
    while (start <= spec.size()) {
        size_t comma = spec.find(',', start);
        std::string tok = spec.substr(start,
            comma == std::string::npos ? std::string::npos : comma - start);
        if (!tok.empty()) {
            Region r{};
            if (parse_region_one(tok, &r, one_based)) out.push_back(std::move(r));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// ── htslib region-string conventions ─────────────────────────────────────────
//
// vv canonicalises every region to UCSC 0-based half-open (see Region /
// parse_region_one / apply_region_modifiers). htslib's region parsers
// (tbx_itr_querys, sam_itr_regarray, hts_parse_region, bcf_itr_querys) instead
// interpret a "chrom:beg-end" STRING as 1-based inclusive at both ends. Feeding
// them a 0-based string therefore shifts every query one base to the left and
// turns the half-open end into an inclusive one — and makes htslib-backed
// formats (tabix BED/VCF/GFF, BAM pileup, BCF) silently disagree with the
// Parquet interval path for the same -r query. Convert at the boundary:
//   UCSC [s, e)  ==  htslib  chrom:(s+1)-e
std::string region_to_htslib(const Region& r) {
    // Whole-chromosome (both bounds open) → bare chrom name.
    if (r.start == INT64_MIN && r.end == INT64_MAX) return r.chrom;
    int64_t beg1 = (r.start == INT64_MIN) ? 1 : r.start + 1;
    if (beg1 < 1) beg1 = 1;
    std::string s = r.chrom + ":" + std::to_string(beg1);
    // Open upper bound → "chrom:beg" (htslib reads it as beg..end-of-chrom).
    if (r.end != INT64_MAX) s += "-" + std::to_string(r.end);
    return s;
}

// Convert a canonical (0-based half-open) comma-separated region list into the
// comma-separated 1-based-inclusive form htslib's parsers expect.
std::string regions_to_htslib(const std::string& canonical) {
    std::string acc;
    for (const auto& r : parse_region_list(canonical)) {
        if (!acc.empty()) acc += ",";
        acc += region_to_htslib(r);
    }
    return acc;
}


// True when htslib finds a tabix / CSI index for `path`. Loading it with
// htslib's logging off keeps its "[E::idx_find_and_load] Could not retrieve
// index file" line off stderr: a missing index is an expected case here.
bool tabix_index_exists(const std::string& path) {
    const int level = hts_get_log_level();
    hts_set_log_level(HTS_LOG_OFF);
    tbx_t* tbx = tbx_index_load(path.c_str());
    hts_set_log_level((htsLogLevel)level);
    if (!tbx) return false;
    tbx_destroy(tbx);
    return true;
}

// -r over a text file that has no tabix index: read every data line and keep
// those overlapping a window, as a tabix query of the same file would. Each
// line's span follows tabix's presets, as 0-based half-open:
//   BED      chrom, start, end (col 1-3; a zero-length interval counts 1 bp)
//   GFF/GTF  seqid, start - 1, end (col 1, 4, 5; 1-based inclusive)
//   VCF      CHROM, POS - 1, POS - 1 + len(REF), or INFO END= when present
//   SAM      RNAME, POS - 1, plus the reference length of CIGAR ('*' = 1)
//   PAF      target name, start, end (col 6, 8, 9)
//   mpileup  chrom, pos - 1, pos
// A window's chromosome also matches its UCSC / Ensembl alias (chr1 / 1).
// A line whose coordinates do not parse is passed on, so the reader reports
// it as it would without -r; blank lines are passed on and skipped there.
