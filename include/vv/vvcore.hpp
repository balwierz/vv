// vvcore.hpp — public surface of the vv reader core (libvvcore).
//
// libvvcore is the headless half of vv: every file-format reader plus the
// Arrow plumbing, the filter engine, and the cell formatters, with no
// ncurses and no main(). It is compiled from the same main.cpp as the CLI,
// guarded by VV_CORE_LIB. The CLI/TUI, the Qt GUI (vvg), and the KF6
// thumbnailer / metadata plugins all consume this header.
//
// Only the surface a frontend needs lives here. Concrete source classes
// (ParquetSource, Hdf5Source, …) stay private to libvvcore — frontends see
// them only through the TabularSource base returned by open_source().
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/type.h>

// ── CLI / reader configuration ───────────────────────────────────────────────
// Knobs consumed by open_source() and the readers. Frontends populate the
// handful they care about (path, region, threads, filter_expr, …) and leave
// the rest at their defaults.
enum class ColorMode { Auto, Always, Never };

struct Config {
    std::string path;                    // first positional (back-compat)
    std::vector<std::string> paths;      // every positional (multi-file TUI)
    std::string region;                  // -r: tabix region(s), e.g. "chr1:1000-2000"
    std::string regions_file;            // --regions-file BED: many windows from a BED
    std::string region_cols;             // --region-cols Chr,Start,End for generic Parquet
    int64_t     slop = 0;                // --slop N: pad each region by N bp
    std::string parquet_out;             // --parquet <file>: write Parquet to this path
    std::string arrow_out;               // --arrow/--feather <file>: write Arrow IPC (Feather v2)
    std::string compression = "zstd";    // --compression: parquet codec
    int         head_rows      = 10;
    bool        head_rows_set  = false;  // true when -n was given explicitly
    int         max_col_w      = 32;
    int         max_cols       = 0;
    int         threads        = 0;      // -@ / --threads (0 = auto-detect)
    int         decode_threads = 0;      // --decode-threads (0 = follow --threads;
                                         // dedicated knob for Arrow's CPU pool
                                         // which handles Parquet column decode)
    bool        no_index       = false;
    ColorMode   color          = ColorMode::Auto;
    char        delimiter      = 0;      // 0 = table/interactive; '\t'/','= delimited
    bool        no_header      = false;
    bool        interactive    = false;  // -i / --interactive
    bool        no_interactive = false;  // --no-interactive
    bool        vertical       = false;  // --vertical (or invoked as `vh`)
    bool        schema_only    = false;  // --schema: print schema + footer, exit
    bool        describe       = false;  // --describe: per-column statistics
    bool        stats_only     = false;  // --stats: Parquet metadata dump (no data read)
    bool        count          = false;  // --count: print row count and exit
    std::string unique_cols;             // --unique COL[,COL,...] : distinct value counts
    int         sample_n       = 0;      // --sample N (reservoir sample of N rows)
    std::string filter_expr;             // --filter "<col> <op> <literal> ..."
    std::string select_cols;             // --select Chr,Start,End (name-based projection)
    bool        json_array     = false;  // --json
    bool        json_lines     = false;  // --ndjson
    bool        md             = false;  // --md (GitHub-flavored markdown table)
    bool        validate       = false;  // --validate (LociSSD invariants check)
    bool        coords_one_based = false; // --coords NCBI (1-based inclusive)
    std::string theme;                    // --theme NAME (empty = "use config-file value or default")
    int         scrolloff  = -1;         // TUI: rows kept between the cell
                                         // cursor and the top/bottom edge.
                                         // -1 = unset (use the built-in
                                         // default); config-file key only.
    int         tail_rows      = 0;      // --tail N
    bool        tail_rows_set  = false;
    bool        decode_pileup  = false;  // --decode-pileup: explode mpileup's
                                         // packed bases column into per-allele
                                         // counts (A/C/G/T/N + ins/del + strand)
    bool        pileup         = false;  // --pileup: BAM/CRAM only — emit
                                         // mpileup-style per-base rows via
                                         // htslib's bam_plp_auto engine
                                         // instead of alignment records
    std::string pileup_ref;              // -f/--fasta: reference FASTA for
                                         // --pileup; enables ref column + the
                                         // ./, match notation (samtools -f)
    bool        heatmap        = false;  // --heatmap: render the numeric matrix
                                         // as a colour heatmap to the terminal
    std::string image_mode;              // --image-mode auto|kitty|sixel|halfblock|ascii
    std::string tab;                     // --tab NAME: view a named component
                                         // tab (AnnData obs/var/X, sheet, …)
};

// ── Cell formatting helpers (defined in libvvcore) ───────────────────────────
// display_width: terminal column width (wcwidth-style: wide CJK/fullwidth/emoji
// count as 2, combining/zero-width as 0), not byte length or codepoint count.
// truncate: smart truncation preserving [first, …] form for collections.
// digits_with_sep: PEP-515 `_` grouping for integers.
// cell_to_string: raw Arrow value → text. cell_to_display_string adds
// digit-grouping for integers. Both are what a GUI model wants for display.
int         display_width(const std::string& s);
std::string truncate(const std::string& s, int max_w);
std::string digits_with_sep(const std::string& s);
std::string cell_to_string(const arrow::Array& arr, int64_t row);
std::string cell_to_display_string(const arrow::Array& arr, int64_t row);

// ── Filter engine ────────────────────────────────────────────────────────────
// The same DSL behind --filter and the TUI's `&` live filter. Parse once
// against the source schema, then evaluate per row. Reused verbatim by the
// GUI's filter bar.
struct FilterAtom {
    int      col_idx = -1;
    // Eq..Ge are the ordering comparisons. The rest are string / set / null
    // predicates: Match/NotMatch take an ECMAScript regex, Contains/StartsWith/
    // EndsWith a substring, In/NotIn a set, IsNull/NotNull no literal at all.
    enum Op { Eq, Ne, Lt, Le, Gt, Ge,
              Match, NotMatch, Contains, NotContains,
              StartsWith, EndsWith, In, NotIn, IsNull, NotNull } op = Eq;
    // K_None: the operator takes no literal (IsNull / NotNull).
    enum Kind { K_Int, K_Double, K_String, K_None } kind = K_String;
    int64_t  i_lit = 0;
    double   f_lit = 0.0;
    std::string s_lit;                    // literal, substring, or regex source
    std::vector<std::string> set_lits;    // In / NotIn members
};
struct FilterExpr {
    // OR of AND clauses; row matches iff some clause's atoms all match.
    std::vector<std::vector<FilterAtom>> groups;
};

// Parse the user's `--filter` expression. Returns true on success and
// populates `out`. On failure, writes a human-readable reason to `err`.
bool parse_filter_expr(const std::string& expr,
                       const arrow::Schema& schema,
                       FilterExpr* out, std::string* err);

// Evaluate a parsed filter over the whole source; returns matching source
// row indices in source order. Drives the GUI's live filter.
class TabularSource;
std::vector<int64_t> filter_rows(TabularSource& src, const FilterExpr& expr);

// ── Per-column statistics ────────────────────────────────────────────────────
// Structured form of what --describe computes, for one column. Backs the
// GUI's stats panel and could back a future structured --describe.
struct ColStats {
    std::string name, type;
    bool        is_numeric = false;
    int64_t     count = 0;            // non-null values
    int64_t     nulls = 0;
    double      min = 0, max = 0, mean = 0;   // numeric only
    std::string s_min, s_max;                 // string only
    std::vector<std::string> distinct;        // up to 16 distinct values
    bool        distinct_overflow = false;    // true if > 16 distinct
    bool        valid = false;
};
ColStats compute_col_stats(TabularSource& src, int src_col);

// ── Source interface ─────────────────────────────────────────────────────────
struct ChunkMeta { int64_t first_row; int64_t num_rows; };

// Abstract interface over every supported format. Frontends drive it
// chunk-by-chunk: total_rows() (−1 while a streaming source is not fully
// scanned), num_chunks()/chunk_meta(i), read_chunk(i, cols, &out) for lazy
// paging, read_first(rows, cols, &out) for fast previews (thumbnails),
// change_slice() for NPZ 3-D+ arrays, and tab_label()/footer()/schema() for
// chrome. Concrete subclasses live inside libvvcore.
class TabularSource {
public:
    virtual ~TabularSource() = default;
    virtual std::shared_ptr<arrow::Schema> schema() const = 0;
    virtual int64_t total_rows() const = 0;     // -1 = not yet fully scanned
    virtual int     num_chunks() const = 0;     // known so far
    virtual ChunkMeta chunk_meta(int i) const = 0;
    // Read chunk i, keeping only col_indices columns.
    virtual arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                                      std::shared_ptr<arrow::Table>* out) = 0;
    // Read only the first `rows` rows with col_indices columns. Default impl
    // reads forward via read_chunk() and concatenates; Parquet overrides with
    // a RecordBatchReader-based fast path so a 10-row preview doesn't decode
    // a whole 1M-row row group.
    virtual arrow::Status read_first(int64_t rows,
                                     const std::vector<int>& col_indices,
                                     std::shared_ptr<arrow::Table>* out) {
        std::shared_ptr<arrow::Table> acc;
        for (int c = 0; ; ++c) {
            ensure(c);
            if (c >= num_chunks()) break;
            std::shared_ptr<arrow::Table> chunk;
            ARROW_RETURN_NOT_OK(read_chunk(c, col_indices, &chunk));
            if (!acc) acc = chunk;
            else {
                auto r = arrow::ConcatenateTables({acc, chunk});
                ARROW_RETURN_NOT_OK(r.status());
                acc = r.ValueOrDie();
            }
            if (acc->num_rows() >= rows) break;
        }
        if (acc && acc->num_rows() > rows) acc = acc->Slice(0, rows);
        *out = acc;
        return arrow::Status::OK();
    }
    // Ensure chunk i is loaded (triggers forward reads for streaming sources).
    virtual void ensure(int i) {}
    // Pin/unpin batch retention. Forward-only streaming sources keep only a
    // bounded trailing window of decoded batches by default (so deep scrolling
    // / G on a huge file doesn't grow RAM without bound); a full-pass operation
    // that must read every row (search, sort, filter, column stats) calls
    // set_retain_all(true) first so nothing it has already passed is evicted.
    // No-op for random-access / in-memory sources. Default: no-op.
    virtual void set_retain_all(bool /*retain*/) {}
    // True if this source has freed any already-read batch under the retention
    // window — i.e. a full-pass result may be incomplete for the freed range.
    // Default: false (nothing is ever evicted).
    virtual bool evicted_any() const { return false; }
    // True when this source actually restricted its scan to Config::region.
    // Only some formats carry an index vv can query (tabix'd text, indexed
    // BAM/CRAM/BCF, bigBed/bigWig, LociSSD, sorted Parquet with chrom/start/
    // end columns); the rest used to ignore `-r` silently and hand back the
    // whole file. A frontend that offers region queries checks this after
    // open_source() and tells the user when the filter was not applied.
    // Default: false (this format has no region index).
    virtual bool region_applied() const { return false; }
    // Sticky status of the underlying streaming read. A source that hits an
    // I/O or parse error mid-stream records it here so callers can tell a
    // complete result apart from a silently truncated one (and exit non-zero).
    // Default: always OK.
    virtual arrow::Status read_status() const { return arrow::Status::OK(); }
    // Step the slice axis for 3-D+ array sources (NPZ today). Default no-op.
    // Returns true if the source rebuilt its table and the TUI should
    // drop cached chunks + reset the viewport.
    virtual bool change_slice(int /*delta*/, bool /*absolute*/,
                                int64_t /*target*/) {
        return false;
    }
    virtual const std::string& path() const = 0;
    // Short label shown on the multi-tab TUI tab bar. Defaults to the
    // basename of path(); sub-tab sources (xlsx sheets, sqlite tables,
    // HDF5 datasets) override to return the sheet/table/dataset name.
    virtual std::string tab_label() const {
        const std::string& p = path();
        auto s = p.rfind('/');
        return (s == std::string::npos) ? p : p.substr(s + 1);
    }
    // One-line footer shown after the table / in the TUI status bar.
    virtual std::string footer() const = 0;
    virtual std::string created_by() const { return ""; }
    // Header lines shown before the table (BED track/browser lines).
    virtual std::vector<std::string> preamble_above() const { return {}; }
    // One-line banner shown prominently on top (non-interactive: above the
    // table via preamble_above(); TUI: a reserved top row). Empty = none.
    // Used for the LociSSD genome assembly / species / element-count summary.
    virtual std::string top_banner() const { return ""; }
    // Meta header lines shown after the schema (VCF/BAM/SAM/GFF).
    virtual std::vector<std::string> preamble_below() const { return {}; }
    // Post-process a cell value for human-readable display (table view and TUI).
    // NOT called for delimited (--csv/--tsv) output.
    virtual std::string format_cell(int /*col_idx*/, std::string val) const { return val; }
    // Minimum display-column width for a given column index (TUI pre-sizes columns from this).
    virtual int min_col_width(int /*col_idx*/) const { return 4; }
    // Column names that should be hidden from human-facing views (table,
    // vertical-head, TUI). Delimited and Parquet output keep all columns.
    // Used e.g. to hide the derived `MaxEndSoFar` column in LociSSD files.
    virtual std::vector<std::string> hidden_for_display() const { return {}; }
    // Sibling "tabs" for multi-tab containers (xlsx/ods sheets, sqlite
    // tables, hdf5/npz datasets). Returns the OTHER tabs beyond this one,
    // each a ready-to-read source sharing the underlying file handle.
    // Default: a single-tab source has no siblings. Frontends concatenate
    // {this, *expand_tabs()...} into their tab strip.
    virtual std::vector<std::unique_ptr<TabularSource>> expand_tabs() const {
        return {};
    }
};

// ── Factory ──────────────────────────────────────────────────────────────────
// The single entry point: dispatch by extension/magic, open the file, and
// return a ready-to-read source. Returns "" on success or a human-readable
// error. Multi-tab containers (xlsx/ods/sqlite/hdf5/npz) return their first
// tab; siblings expand via the WorkbookSource / SqliteSource hooks.
std::string open_source(const std::string& path, const Config& cfg,
                        std::unique_ptr<TabularSource>* out);

// Canonicalise region inputs in `cfg` *before* open_source(): folds --coords
// (NCBI↔UCSC), --regions-file, and --slop into cfg.region as a UCSC 0-based
// half-open comma list and clears cfg.coords_one_based, so downstream readers
// are coordinate-agnostic. A frontend offering region queries calls this once
// before open_source(). Returns "" on success or a human-readable error.
std::string apply_region_modifiers(Config& cfg);
