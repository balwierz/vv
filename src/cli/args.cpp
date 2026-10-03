// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Command line: the format registry, --formats, --help and parse_args().

#include "internal.hpp"

// ── Format registry ──────────────────────────────────────────────────────────
//
// One authoritative list of what vv reads. Before this table the same
// information was restated in seven places — print_usage(), README.md,
// docs/USAGE.md, man/vv.1, three shell completions and four KDE manifests —
// and they had already drifted apart: `.npz` was missing from all three
// completions, `.arrow`/`.feather` from --help entirely, `.fods` and
// `.ffn`/`.frn` from most of them, while README documented `.npy` and the
// KDE .desktop claimed `.xls` — neither of which vv can open.
//
// `--formats [--json]` prints this table, and tests/run_tests.sh diffs the
// completions against it so the next drift fails CI instead of shipping.
//
// exts: canonical spelling, space-separated. Matching is case-insensitive
// (fends_ci), so `.bigBed` also matches `.bigbed` — but shell globs are
// case-sensitive, which is why the completion generator emits both.
struct FormatInfo {
    const char* name;      // human label
    const char* exts;      // space-separated, leading dot, canonical case
    const char* reader;    // the source class it dispatches to
    bool gz;               // .gz variants dispatched too
    bool region;           // -r honoured (see `region_note` for the condition)
    bool tabs;             // expands into component tabs (--tab / TUI tabs)
    bool streaming;        // forward-only reader (vs random access)
    bool magic;            // also detected by magic bytes on an unknown ext
    const char* region_note;
};

static const FormatInfo kFormats[] = {
  {"Apache Parquet", ".parquet", "ParquetSource",
   false, true,  false, false, true,  "needs chrom/start/end columns (--region-cols)"},
  {"LociSSD", ".lociss", "LocissV4Source / ParquetSource",
   false, true,  false, false, true,  "v3 via row-group stats, v4 via the zone map"},
  {"Arrow IPC", ".arrow", "IpcSource",
   false, false, false, false, true,  ""},
  {"Feather", ".feather", "IpcSource",
   false, false, false, false, true,  ""},
  {"Apache ORC", ".orc", "OrcSource",
   false, false, false, false, false, ""},
  {"BAM / CRAM alignments", ".bam .cram", "BamSource / BamPileupSource",
   false, true,  false, true,  false, "needs a .bai/.csi/.crai index"},
  {"SAM alignments (text)", ".sam", "DelimitedSource",
   false, false, false, true,  false, "no index; convert with `samtools view -b`"},
  {"BCF", ".bcf", "BcfSource",
   false, true,  false, true,  false, "needs a .csi/.tbi index"},
  {"VCF", ".vcf", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix"},
  {"GFF / GFF3 / GTF", ".gff .gff3 .gtf", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix"},
  {"BED", ".bed", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix"},
  {"ENCODE peak / signal", ".narrowPeak .broadPeak .gappedPeak .bedGraph .bg .tagAlign",
   "DelimitedSource", true, true, false, true, false, "bgzip + tabix"},
  {"Delimited text", ".tsv .csv", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix"},
  {"samtools mpileup", ".pileup .mpileup .pile", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix"},
  {"PAF (minimap2)", ".paf", "DelimitedSource",
   true,  false, false, true,  false, ""},
  {"Arrow IPC stream", ".arrows", "IpcStreamSource",
   false, false, false, true,  true,  ""},
  {"MatrixMarket (sparse matrix)", ".mtx", "DelimitedSource",
   true,  false, false, true,  false, ""},
  {"PLINK variant / sample tables", ".bim .fam .pvar .psam", "DelimitedSource",
   true,  false, false, true,  false, ""},
  {"R data", ".rds .RData .rda", "RdataSource",
   false, false, true,  false, false, ""},
  {"UCSC wiggle", ".wig", "DelimitedSource",
   true,  false, false, true,  false, ""},
  {"BLAST / DIAMOND tabular", ".m8 .blast6 .outfmt6", "DelimitedSource",
   true,  false, false, true,  false, ""},
  {"Genomics TSV layouts", ".bedpe .pairs .gct .maf", "DelimitedSource",
   true,  false, false, true,  false, ""},
  {"FASTA", ".fa .fasta .fna .faa .ffn .frn", "FastxSource",
   true,  false, false, true,  false, ""},
  {"FASTQ", ".fq .fastq", "FastxSource",
   true,  false, false, true,  false, ""},
  {"UCSC bigBed / bigWig", ".bb .bigBed .bw .bigWig", "BigSource",
   false, true,  false, true,  false, "native block-level overlap; no sidecar index"},
  {"UCSC 2bit", ".2bit", "TwoBitSource",
   false, false, false, false, false, ""},
  {"SQLite", ".sqlite .sqlite3 .db", "SqliteSource",
   false, false, true,  true,  false, ""},
  {"Excel workbook", ".xlsx .xlsm", "XlsxSource",
   false, false, true,  false, false, ""},
  {"OpenDocument spreadsheet", ".ods .fods", "OdsSource",
   false, false, true,  false, false, ""},
  {"HDF5 / AnnData / Loom", ".h5ad .h5 .hdf5 .loom .h5mu .h5seurat .fast5 .nc .nc4", "Hdf5Source",
   false, false, true,  false, false, ""},
  {"NumPy archive", ".npz", "NpzSource",
   false, false, true,  false, false, ""},
  {"NumPy array", ".npy", "NpzSource",
   false, false, false, false, false, ""},
  {"Markdown", ".md .markdown .mdown .mkd", "md4c renderer",
   false, false, false, false, false, ""},
  {"JSON / NDJSON", ".json .ndjson .jsonl .geojson .ipynb .har", "JsonSource",
   true,  false, false, true,  false, ""},
  // Plain text is last on purpose: it is the fallback, and any file no other
  // row claims is content-sniffed into it. The extension list is short by
  // design — .py / .c / .conf / .toml reach the same reader through the
  // sniff, and claiming them here would advertise vv as a code viewer.
  {"Plain text", ".txt .text .log", "TextSource",
   true,  false, false, true,  true,  ""},
};
static constexpr size_t kNumFormats = sizeof(kFormats) / sizeof(kFormats[0]);

// Split a FormatInfo::exts blob into individual extensions.
static std::vector<std::string> format_ext_list(const FormatInfo& f) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = f.exts; ; ++p) {
        if (*p == ' ' || *p == '\0') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur += *p;
        }
    }
    return out;
}

// Defined with the other JSON helpers further down.

// `--formats` / `--formats --json`: print the registry. The JSON form is what
// the CI drift check and the completion generator consume.
void print_formats(bool as_json) {
    if (as_json) {
        std::printf("[");
        for (size_t i = 0; i < kNumFormats; ++i) {
            const FormatInfo& f = kFormats[i];
            if (i) std::printf(", ");
            std::printf("{\"name\": ");
            json_emit_string(f.name);
            std::printf(", \"reader\": ");
            json_emit_string(f.reader);
            std::printf(", \"extensions\": [");
            auto exts = format_ext_list(f);
            for (size_t k = 0; k < exts.size(); ++k) {
                if (k) std::printf(", ");
                json_emit_string(exts[k]);
            }
            std::printf("], \"gz\": %s", f.gz ? "true" : "false");
            std::printf(", \"region\": %s", f.region ? "true" : "false");
            std::printf(", \"tabs\": %s", f.tabs ? "true" : "false");
            std::printf(", \"streaming\": %s", f.streaming ? "true" : "false");
            std::printf(", \"magic\": %s", f.magic ? "true" : "false");
            if (*f.region_note) {
                std::printf(", \"region_note\": ");
                json_emit_string(f.region_note);
            }
            std::printf("}");
        }
        std::printf("]\n");
        return;
    }
    size_t w = 4;
    for (const auto& f : kFormats) w = std::max(w, std::strlen(f.name));
    std::printf("%-*s  %-8s %-6s %-4s %-9s %s\n",
                (int)w, "Format", "gz", "region", "tabs", "streaming", "extensions");
    for (const auto& f : kFormats) {
        std::printf("%-*s  %-8s %-6s %-4s %-9s %s\n",
                    (int)w, f.name,
                    f.gz     ? "yes" : "-",
                    f.region ? "yes" : "-",
                    f.tabs   ? "yes" : "-",
                    f.streaming ? "stream" : "random",
                    f.exts);
    }
    std::printf(
        "\ngz: reads compressed input — gzip / bgzip, zstandard, bzip2 and xz\n"
        "(.gz .bgz .zst .bz2 .xz), detected by content. Range queries (-r) need\n"
        "bgzip + a tabix index and so require gzip.\n");
}

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "vv -- universal genomic file viewer\n"
        "\nUsage: %s [options] <file>\n"
        "\nSupported formats:\n"
        "  .parquet\n"
        "  .arrow  .feather          Arrow IPC / Feather (v1 and v2)\n"
        "  .arrows                   Arrow IPC stream (streamed; also on stdin / a pipe)\n"
        "  .lociss                     LociSSD sorted-interval — v3 Parquet (manifest\n"
        "                              in KV) or v4 \"colblock\" binary; dispatched by magic\n"
        "  .bam  .cram                  binary/compressed sequence alignments (htslib)\n"
        "  .sam                        text sequence alignments\n"
        "  .vcf  .vcf.gz               variant calls\n"
        "  .gff  .gff3  .gtf           genome annotations\n"
        "  .bed  .tsv  .csv            intervals, delimited tables\n"
        "  .pileup  .mpileup  .pile        samtools mpileup output (single- or multi-sample)\n"
        "  .narrowPeak  .broadPeak  .gappedPeak  .bedGraph  .bg  .tagAlign\n"
        "                              ENCODE peak / signal formats (BED+typed cols)\n"
        "  .bb  .bigBed                UCSC bigBed (libBigWig; autoSql columns)\n"
        "  .bw  .bigWig                UCSC bigWig (libBigWig)\n"
        "  .wig                        UCSC wiggle (fixedStep / variableStep → intervals)\n"
        "  .2bit                       UCSC 2bit (sequence index: name/length/blocks)\n"
        "  .sqlite  .sqlite3  .db      SQLite database (each table → one TUI tab)\n"
        "  .xlsx  .xlsm                Excel spreadsheet (each sheet → one TUI tab)\n"
        "  .ods  .fods                 OpenDocument spreadsheet, zipped or flat\n"
        "                              XML (each sheet → one TUI tab)\n"
        "  .h5ad                       AnnData (single-cell) — obs / var / X / obsm tabs\n"
        "  .h5  .hdf5  .loom           generic HDF5 — hierarchy tab + per-dataset tabs\n"
        "  .h5mu                       MuData — joint obs / var + each modality's AnnData tabs\n"
        "  .h5seurat  .fast5  .nc  .nc4  HDF5-based: h5Seurat, Nanopore FAST5, NetCDF-4\n"
        "  .npz                        NumPy archive — summary tab + per-array tabs (3-D+ scrubs via [/])\n"
        "  .npy                        NumPy single array\n"
        "  .rds  .RData  .rda          R data frames / vectors / matrices (one tab per\n"
        "                              object; factors, Date, POSIXct, row names)\n"
        "  .orc                        Apache ORC (columnar; one stripe → one chunk)\n"
        "  .md  .markdown  .mdown  .mkd\n"
        "                              CommonMark + GFM markdown (renders as ANSI;\n"
        "                              GFM tables routed through the table renderer)\n"
        "  .fa  .fasta  .fna  .faa  .ffn  .frn\n"
        "                              sequences (FASTA)\n"
        "  .fq  .fastq                 sequencing reads (FASTQ)\n"
        "  .bcf                        binary VCF (htslib)\n"
        "  .paf  .paf.gz               minimap2 pairwise alignments\n"
        "  .mtx  .mtx.gz               MatrixMarket sparse matrix (row, col, value; 0-based)\n"
        "  .bedpe  .pairs  .gct  .maf  BEDPE, 4DN pairs, GenePattern GCT, mutation MAF\n"
        "                              (TSV with their own headers)\n"
        "  .m8  .blast6  .outfmt6      BLAST / DIAMOND tabular output (-outfmt 6;\n"
        "                              the 12 standard columns named)\n"
        "  .bim  .fam  .pvar  .psam    PLINK variant / sample tables (a PLINK .bed /\n"
        "                              .pgen genotype file is refused, with the\n"
        "                              plink2 command that exports it to VCF)\n"
        "  .json  .ndjson  .jsonl      JSON documents (also on stdin;\n"
        "  .geojson  .ipynb  .har      GeoJSON, Jupyter notebooks, HTTP archives):\n"
        "                              a folding tree viewer on a terminal, printed\n"
        "                              re-indented on a pipe; a table of records with\n"
        "                              any table flag (see JSON below)\n"
        "  .txt  .text  .log           plain text (viewed like less -SN,\n"
        "                              not tabulated). The fallback for any file\n"
        "                              no other format claims.\n"
        "  Text formats may be compressed: .gz / .bgz, .zst, .bz2, .xz (by content).\n"
        "  -                           read from stdin (decompressed as above). Text\n"
        "                              streams in; a binary format (Parquet, Arrow,\n"
        "                              BAM, …) is copied to a temporary file first.\n"
        "                              Process substitution works the same:\n"
        "                              vv <(zcat x.parquet.gz)\n"
        "  DIR/                        a directory: concatenate the data files under\n"
        "                              it (Parquet/Arrow/ORC/CSV/TSV/JSON). Hive\n"
        "                              key=value/ path parts become columns\n"
        "  (`vv --formats` prints this table with capability columns;\n"
        "   add --json for the machine-readable form)\n"
        "  (unknown extensions: identified by magic bytes, else sniffed as text;\n"
        "   binary files are refused — vv has no hex view)\n"
        "\nInteractive viewer (default when stdout is a terminal):\n"
        "  -i / --interactive  open the ncurses row browser; --filter, --select and\n"
        "                      --sort set its opening view (& / Esc / u change it)\n"
        "  --table / -t        force plain table output (also --no-interactive)\n"        "  --text              read the file as plain text whatever its extension\n"
        "  -d, --in-delimiter <sep>\n"
        "                      read the input with this field separator, overriding\n"
        "                      the extension (a single char or: tab, space, comma,\n"
        "                      semicolon, pipe). E.g. -d space for R write.table().\n"
        "  --header <mode>     is the CSV/TSV first row a header? auto (default,\n"
        "                      detects it from the data), on (force), off (no header;\n"
        "                      columns are auto-named f0, f1, …)\n"
        "  Keys: arrows/hjkl move the cell cursor, PgUp/PgDn, g/G, /:search,\n"
        "        S:column-stats, F:value counts (Enter filters to one),\n"
        "        s:sort by current column (u clears),\n"
        "        &:live filter, c:show/hide columns, y:copy cell (OSC52),\n"
        "        T:pick a theme (saved to ~/.config/vv/config),\n"
        "        ::command line (:<N> jump, :q quit, :theme NAME),\n"
        "        Tab / Shift-Tab: switch files (with multiple positionals),\n"
        "        H/F1: in-app help, q quit\n"
        "\nJSON documents (.json / .ndjson / .jsonl, or JSON on stdin):\n"
        "  On a terminal a JSON file opens in a folding tree viewer; on a pipe\n"
        "  it prints re-indented, two spaces, scalars as written (like jq .).\n"
        "  A table flag (--tsv, --parquet, --filter, -n, --table, ...) reads it\n"
        "  as a table of records instead.\n"
        "  --tree              the tree viewer (also a JSON file of any extension)\n"
        "  --pretty            print the document re-indented (also on a terminal)\n"
        "  --json-paths        print one `path = value` line per leaf, with jq\n"
        "                      paths: .a.b[0] = 1, .[\"a b\"] = \"x\" (NDJSON: .[i])\n"
        "  --no-tree           read JSON as a table of records (config file:\n"
        "                      json_view = table makes that the default)\n"
        "  Tree keys: j/k move, h/l collapse/expand (h: to the parent), Space\n"
        "        toggle, J/K next/previous sibling, e/E c/C expand/collapse (the\n"
        "        node / everything inside), 1-9 0 fold to depth N, /:search,\n"
        "        y/p/Y copy the value / jq path / key, Enter: full value,\n"
        "        t: table view (t comes back), H: help, q quit\n"
        "\nTable options:\n"
        "  -n <rows>           rows to display  (default: 10, 0 = all)\n"
        "  --tail <N>          show the last N rows instead of the first N\n"
        "  --sort <col>[:desc]  order rows by a column before output (asc\n"
        "                      default; numeric columns sort numerically,\n"
        "                      others by text; nulls last). Loads the file\n"
        "                      into memory.\n"
        "  -w <width>          max cell width   (default: 32)\n"
        "  -c <cols>           max columns to show (default: all)\n"
        "  --select <terms>    project columns. Comma-separated; output follows\n"
        "                      the given order, so it also reorders. Terms:\n"
        "                        Chr        an exact column name (always wins)\n"
        "                        chr*       a glob (* and ?)\n"
        "                        2-4, 5-    a 1-based inclusive index range\n"
        "                        @numeric   a type class: @numeric @string\n"
        "                                   @list @bool @temporal\n"
        "                        !TERM      exclude everything TERM matches\n"
        "                      e.g. --select 'chr*,!*_pct'  (quote it — the\n"
        "                      shell would otherwise expand * itself)\n"
        "  --filter <expr>     keep rows matching: <col> <op> <value>, joined by\n"
        "                      AND / OR. Operators:\n"
        "                        == != < <= > >=   compare\n"
        "                        ~  !~             regex (ECMAScript), unanchored\n"
        "                        contains startswith endswith\n"
        "                        in (a, b, c)      set membership; `not in` too\n"
        "                        in @ids.txt       members from a file, one per\n"
        "                                          line (first tab field; compressed too)\n"
        "                        is null / is not null\n"
        "                        has / lacks F,G   bits of an integer column:\n"
        "                                          all set / none set. SAM FLAG\n"
        "                                          names (UNMAP, SECONDARY, DUP,\n"
        "                                          PAIRED, READ1, …) or numbers\n"
        "                      a date / timestamp column takes a quoted date:\n"
        "                        day > \"2024-01-01\", ts < \"2024-06-15 12:30Z\"\n"
        "                      a boolean column takes true / false; a float32\n"
        "                      column compares at float32 precision\n"
        "                      a column name with spaces or symbols goes in\n"
        "                      backticks: `Sample ID` == \"S1\"  (`` for a `)\n"
        "                      e.g. --filter 'Score > 0.5'\n"
        "                           --filter 'FILTER is null OR Gene ~ \"^BRCA\"'\n"
        "                           --filter 'FLAG lacks UNMAP,SECONDARY,DUP'\n"
        "  --schema            print schema + file metadata and exit\n"
        "                      (with --json: machine-readable; `rows` is null\n"
        "                      when the file has not been fully scanned)\n"
        "  --expand <col>      unpack a packed key=value column into real\n"
        "                      columns, appended to the schema — VCF INFO,\n"
        "                      GFF/GTF attributes. The keys then work with\n"
        "                      --select / --filter / --parquet like any other\n"
        "                      column, e.g.\n"
        "                        vv v.vcf --expand INFO --filter \'AF > 0.05\'\n"
        "                      VCF types come from the ##INFO declarations.\n"
        "                      GFF/GTF declares nothing, so keys are taken\n"
        "                      from the first chunk: a -n preview and a full\n"
        "                      scan CAN disagree on the column set.\n"
        "  --flatten           struct columns become one column per leaf, named\n"
        "                      by path (st.a, st.b.c); lists and maps are kept\n"
        "  --samples <how>     VCF/BCF sample columns: struct (default; one\n"
        "                      struct per sample typed from ##FORMAT, e.g.\n"
        "                      {GT: 0/1, AD: [5, 6], DP: 11}; --flatten gives\n"
        "                      S1.GT, S1.DP, ...), long (one row per record x\n"
        "                      sample: sample, GT, AD, DP, ...; --filter 'DP < 10'\n"
        "                      reaches every sample), text (the packed strings)\n"
        "  --matrix <how>      HDF5 / AnnData / Loom / Cell Ranger matrix tabs:\n"
        "                      wide (default; cells x genes) or long (one row per\n"
        "                      stored non-zero value: obs, var, value, named\n"
        "                      after the file's indexes), in the viewer and\n"
        "                      every export\n"
        "  --list-columns      column names, one per line\n"
        "  --list-tabs         component tab labels, one per line\n"
        "  --formats           the supported-format table (add --json)\n"
        "  --tab <name>        view a named component tab (AnnData obs/var/X,\n"
        "                      a workbook sheet, …) instead of the first; e.g.\n"
        "                      `vv cells.h5ad --tab obs -n 20`\n"
        "  --describe          per-column statistics and exit (add --json /\n"
        "                      --ndjson for machine-readable stats)\n"
        "  --count             print the row count and exit (honours -r and\n"
        "                      --filter). An indexed BAM / BCF / VCF.gz is\n"
        "                      counted from its index when nothing filters\n"
        "  --stats             print Parquet metadata footer (row groups, codecs,\n"
        "                      per-column sizes, key-value metadata) without\n"
        "                      reading data; exit (add --json for JSON)\n"
        "  --contigs           BAM/CRAM/SAM, VCF/BCF: list the reference sequences\n"
        "                      (name, length) from the header and name the assembly\n"
        "                      (GRCh38, mm10, …). Reads no records; composes with\n"
        "                      --tsv / --json / --sort / --filter. With an index:\n"
        "                      mapped / unmapped reads (BAM) or records (VCF / BCF)\n"
        "                      per sequence, as samtools idxstats\n"
        "  --seq-stats         FASTA/FASTQ: one-row summary — record count, total /\n"
        "                      min / mean / max length, N50, GC %%, and for FASTQ\n"
        "                      Q20 / Q30 %%; composes with --tsv / --json\n"
        "  --gt-stats          VCF/BCF: add per-variant genotype summary columns\n"
        "                      over the samples — n_called/n_het/n_hom_ref/\n"
        "                      n_hom_alt/n_missing, AC/AN/AF, call_rate. A fixed\n"
        "                      set whatever the sample count; `--filter 'AF > 0.05'`\n"
        "                      and `--sort call_rate` work on them\n"
        "  --unique <cols>     comma-separated columns: print distinct-value counts\n"
        "  --distinct          drop duplicate rows (SQL SELECT DISTINCT), keeping\n"
        "                      the first of each. Over the shown columns, so\n"
        "                      `--select chrom --distinct` lists distinct chroms;\n"
        "                      honours --filter, composes with --sort / --count\n"
        "  --sample <N>        reservoir-sample N rows uniformly instead of head-N\n"
        "  --validate          check LociSSD invariants (sort order, MaxEndSoFar,\n"
        "                      manifest vs. data); exit non-zero on failure\n"
        "  --pileup            BAM/CRAM only: emit mpileup-style per-base rows\n"
        "                      from the alignments via htslib's pileup engine\n"
        "                      (equivalent to `samtools mpileup`, no BAQ), with\n"
        "                      its default read / base filters, overridable:\n"
        "    --exclude-flags L (--ff) skip reads with any flag in L\n"
        "                      (default UNMAP,SECONDARY,QCFAIL,DUP; 0 = none)\n"
        "    --require-flags L (--rf) keep only reads with every flag in L\n"
        "    --min-mapq N      skip reads below mapping quality N (default 0)\n"
        "    --min-bq N        drop bases below base quality N (default 13)\n"
        "    --count-orphans   keep paired reads not properly paired (-A)\n"
        "    --ignore-overlaps no mate-overlap quality merging (-x)\n"
        "  -f, --fasta <ref>   reference FASTA (needs .fai). With --pileup it\n"
        "                      fills the ref column and renders matches as\n"
        "                      . / , like `samtools mpileup -f`; on a CRAM\n"
        "                      input it supplies the reference needed to\n"
        "                      decode the reads (otherwise htslib falls back\n"
        "                      to $REF_PATH / $REF_CACHE)\n"
        "  --decode-pileup     mpileup only: replace the packed bases/quals\n"
        "                      columns with typed per-allele counts\n"
        "                      (A, C, G, T, N, del, ins, fwd, rev, mean_qual)\n"
        "  --tags <list>       BAM/CRAM/SAM and PAF: add one column per named aux\n"
        "                      tag (comma-separated 2-char SAM tags, e.g.\n"
        "                      --tags NM,AS,RG). Column type follows the tag's\n"
        "                      SAM type (i->int, f->float, else string), so\n"
        "                      `--filter 'NM <= 2'` compares numbers; a read\n"
        "                      without the tag is null\n"
        "  --no-index          suppress the row-index column\n"
        "  --color[=WHEN]      colorize output: auto (default), always, never\n"
        "  --theme <name>      color palette: default, dark, light,\n"
        "                      solarized-dark, solarized-light (default = default)\n"
        "  --box <style>       table frame: unicode (default) or ascii (+-| for a\n"
        "                      C locale, an ASCII-only pipe, or plain-text paste).\n"
        "                      Auto-selects ascii when the locale is not UTF-8\n"
        "  --vertical          \"vertical head\": transpose the preview so each\n"
        "                      field is a row; show as many records per line as\n"
        "                      fit. Implies --no-interactive. Default when the\n"
        "                      binary is invoked as `vh`.\n"
        "\nVisualization (replaces table view):\n"
        "  --heatmap           render the numeric columns as a colour heatmap in\n"
        "                      the terminal (rows x numeric-columns, globally\n"
        "                      normalised). Writes a plain ASCII grid when stdout\n"
        "                      is not a terminal.\n"
        "  --image-mode <how>  heatmap backend: auto (default: kitty graphics in\n"
        "                      kitty, iterm in iTerm2 / WezTerm, else halfblock),\n"
        "                      kitty, iterm, sixel, halfblock, ascii\n"
        "\nDelimited output (replaces table view):\n"
        "  --tsv               write tab-separated values to stdout\n"
        "  --csv               write comma-separated values to stdout\n"
        "  --json              write a JSON array of row objects to stdout\n"
        "  --ndjson            write one JSON object per line (JSON Lines)\n"
        "  --md / --markdown   write a GitHub-flavored markdown table\n"
        "  --delimiter <sep>   write with a custom single-character delimiter\n"
        "  --no-header         omit the header row\n"
        "  (-n defaults to all rows in this mode; -c still applies)\n"
        "\nParquet / Arrow output (replaces table view):\n"
        "  --parquet <file>    write a Parquet file at <file> (or `-` for stdout)\n"
        "  --arrow, --feather <file>\n"
        "                      write an Arrow IPC file (Feather v2) at <file>\n"
        "                      (or `-` for stdout)\n"
        "  --compression <c>   Parquet: zstd (default), snappy, gzip, lz4, none;\n"
        "                      Arrow/Feather: zstd (default), lz4, none\n"
        "\nRange queries:\n"
        "  -r / --region <REGION>   e.g. chr1:1000-2000  (multiple comma-separated)\n"
        "  --window <REGION>        alias of -r for LociSSD readers' muscle memory\n"
        "  --regions-file <BED>     read additional windows from a BED file's\n"
        "                           first three columns\n"
        "  --region-cols <names>    chrom/start/end column names for plain\n"
        "                           Parquet (3 comma-separated names; default:\n"
        "                           auto-detect Chromosome/Chrom/Chr + Start/POS\n"
        "                           + End/Stop)\n"
        "  --slop <N>               pad each window by N bp on both sides\n"
        "  --coords <kind>          coordinate convention for -r: UCSC\n"
        "                           (default; 0-based half-open, as in BED)\n"
        "                           or NCBI (1-based inclusive, as in GenBank,\n"
        "                           VCF, GFF, and the samtools/tabix CLI)\n"
        "  Supported on indexed BAM/CRAM (.bai/.csi/.crai), tabix-indexed\n"
        "  VCF/BED/GFF/TSV, indexed BCF (.csi/.tbi), LociSSD (.lociss),\n"
        "  plain sorted Parquet with chrom/start/end columns, and\n"
        "  bigBed/bigWig. Unindexed BED/VCF/GFF/SAM/PAF/mpileup text is\n"
        "  read in full and filtered, with a note on stderr; other formats\n"
        "  with no region index warn on stderr and show the whole file.\n"
        "  Coordinates follow the UCSC convention\n"
        "  (0-based half-open) by default; pass --coords NCBI for 1-based\n"
        "  inclusive (samtools/tabix style).\n"
        "\nPerformance:\n"
        "  -@ / --threads <N>  worker threads for I/O and decode (0 = auto)\n"
        "  --decode-threads <N>  Arrow CPU thread pool size for Parquet /\n"
        "                       CSV decode (0 = follow --threads; useful when\n"
        "                       cold reads bottleneck on column decompression)\n"
        "\n  -h / --help         show this help\n"
        "  -V / --version      print version and exit\n",
        prog);
}

// Case-insensitive suffix test; defined with the other path helpers below.



// A SAM flag list as samtools --ff / --rf take it: comma-separated names
// (any case) and / or numbers (4, 0x704), OR'ed together. Sets *err and returns
// 0 on an unknown member.
static int parse_sam_flag_list(const std::string& list, std::string* err) {
    int mask = 0;
    size_t p = 0;
    while (p <= list.size()) {
        size_t c = list.find(',', p);
        std::string t = list.substr(p, c == std::string::npos ? std::string::npos : c - p);
        while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(0, 1);
        while (!t.empty() && std::isspace((unsigned char)t.back()))  t.pop_back();
        if (t.empty()) { *err = "empty member in flag list '" + list + "'"; return 0; }
        int bit = -1;
        for (const auto& [nm, v] : kSamFlagNames)
            if (strcasecmp(t.c_str(), nm) == 0) { bit = v; break; }
        if (bit < 0) {
            char* end = nullptr;
            long v = std::strtol(t.c_str(), &end, 0);
            if (end && !*end && v >= 0 && v <= 0xffff) bit = (int)v;
        }
        if (bit < 0) {
            std::string names;
            for (const auto& [nm, v] : kSamFlagNames) names += std::string(names.empty() ? "" : " ") + nm;
            *err = "unknown flag '" + t + "' (a number, or one of: " + names + ")";
            return 0;
        }
        mask |= bit;
        if (c == std::string::npos) break;
        p = c + 1;
    }
    return mask;
}

Config parse_args(int argc, char** argv) {
    const char* pileup_opt = nullptr;   // a --pileup filter option, if given
    Config cfg;
    // If invoked as `vh` (a symlink/copy of vv), default to vertical-head mode:
    // a "head"-style preview transposed so wide tables fit without horizontal
    // scrolling. Implies --no-interactive.
    if (argc > 0) {
        const char* base = std::strrchr(argv[0], '/');
        base = base ? base + 1 : argv[0];
        if (std::strcmp(base, "vh") == 0) {
            cfg.vertical       = true;
            cfg.no_interactive = true;
            g_vertical_from_argv0 = true;
        }
    }
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            print_usage(argv[0]); std::exit(0);
        } else if (!std::strcmp(argv[i], "-V") || !std::strcmp(argv[i], "--version")) {
            std::printf("vv %s\n", kVersion); std::exit(0);
        } else if (!std::strcmp(argv[i], "--no-index")) {
            cfg.no_index = true;
        } else if (!std::strcmp(argv[i], "--no-header")) {
            cfg.no_header = true;
        } else if (!std::strcmp(argv[i], "-i") || !std::strcmp(argv[i], "--interactive")) {
            cfg.interactive = true;
        } else if (!std::strcmp(argv[i], "--no-interactive") ||
                   !std::strcmp(argv[i], "--table") ||
                   !std::strcmp(argv[i], "-t")) {
            cfg.no_interactive = true;
        } else if (!std::strcmp(argv[i], "--vertical")) {
            cfg.vertical       = true;
            cfg.no_interactive = true;
        } else if (!std::strcmp(argv[i], "-n") && i + 1 < argc) {
            cfg.head_rows     = std::atoi(argv[++i]);
            cfg.head_rows_set = true;
        } else if (!std::strcmp(argv[i], "-w") && i + 1 < argc) {
            cfg.max_col_w = std::max(4, std::atoi(argv[++i]));
            cfg.max_col_w_set = true;
        } else if (!std::strcmp(argv[i], "-c") && i + 1 < argc) {
            cfg.max_cols = std::atoi(argv[++i]);
        } else if ((!std::strcmp(argv[i], "-r") ||
                    !std::strcmp(argv[i], "--region") ||
                    !std::strcmp(argv[i], "--window")) && i + 1 < argc) {
            cfg.region = argv[++i];
        } else if (!std::strcmp(argv[i], "--regions-file") && i + 1 < argc) {
            cfg.regions_file = argv[++i];
        } else if (!std::strcmp(argv[i], "--region-cols") && i + 1 < argc) {
            cfg.region_cols = argv[++i];
        } else if (!std::strcmp(argv[i], "--slop") && i + 1 < argc) {
            cfg.slop = (int64_t)std::atoll(argv[++i]);
        } else if (!std::strcmp(argv[i], "--coords") && i + 1 < argc) {
            const char* v = argv[++i];
            // NCBI / GenBank / tabix / VCF / samtools: 1-based inclusive
            if (!std::strcmp(v, "ncbi") || !std::strcmp(v, "NCBI") ||
                !std::strcmp(v, "genbank") || !std::strcmp(v, "GenBank") ||
                !std::strcmp(v, "1-based") || !std::strcmp(v, "1based") ||
                !std::strcmp(v, "tabix"))
                cfg.coords_one_based = true;
            // UCSC / Kent / BED tools: 0-based half-open
            else if (!std::strcmp(v, "ucsc") || !std::strcmp(v, "UCSC") ||
                     !std::strcmp(v, "kent") || !std::strcmp(v, "Kent") ||
                     !std::strcmp(v, "0-based") || !std::strcmp(v, "0based") ||
                     !std::strcmp(v, "bed"))
                cfg.coords_one_based = false;
            else {
                std::fprintf(stderr,
                    "--coords: expected 'UCSC' (0-based half-open, default) "
                    "or 'NCBI' (1-based inclusive), got %s\n", v);
                std::exit(2);
            }
        } else if (!std::strcmp(argv[i], "--tail") && i + 1 < argc) {
            cfg.tail_rows     = std::max(0, std::atoi(argv[++i]));
            cfg.tail_rows_set = true;
        } else if (!std::strcmp(argv[i], "--sort") && i + 1 < argc) {
            // COL, or COL:asc / COL:desc. Only a trailing :asc/:desc is a
            // direction — a column name may itself contain a colon.
            std::string v = argv[++i];
            auto colon = v.rfind(':');
            if (colon != std::string::npos) {
                std::string suf = v.substr(colon + 1);
                if      (suf == "desc") { cfg.sort_desc = true;  v.resize(colon); }
                else if (suf == "asc")  { cfg.sort_desc = false; v.resize(colon); }
            }
            cfg.sort_col = v;
        } else if (!std::strcmp(argv[i], "--tags") && i + 1 < argc) {
            cfg.bam_tags = argv[++i];
        } else if ((!std::strcmp(argv[i], "-@") ||
                    !std::strcmp(argv[i], "--threads")) && i + 1 < argc) {
            cfg.threads = std::max(0, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--decode-threads") && i + 1 < argc) {
            cfg.decode_threads = std::max(0, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--parquet") && i + 1 < argc) {
            cfg.parquet_out = argv[++i];
        } else if ((!std::strcmp(argv[i], "--arrow") ||
                    !std::strcmp(argv[i], "--feather")) && i + 1 < argc) {
            cfg.arrow_out = argv[++i];
        } else if (!std::strcmp(argv[i], "--compression") && i + 1 < argc) {
            cfg.compression = argv[++i];
        } else if (!std::strcmp(argv[i], "--expand") && i + 1 < argc) {
            cfg.expand_col = argv[++i];
        } else if (!std::strcmp(argv[i], "--list-columns")) {
            cfg.list_columns = true;
        } else if (!std::strcmp(argv[i], "--list-tabs")) {
            cfg.list_tabs = true;
        } else if (!std::strcmp(argv[i], "--formats")) {
            cfg.list_formats = true;
        } else if (!std::strcmp(argv[i], "--schema")) {
            cfg.schema_only = true;
        } else if (!std::strcmp(argv[i], "--describe")) {
            cfg.describe = true;
        } else if (!std::strcmp(argv[i], "--count")) {
            cfg.count = true;
        } else if (!std::strcmp(argv[i], "--stats")) {
            cfg.stats_only = true;
        } else if (!std::strcmp(argv[i], "--contigs")) {
            cfg.contigs = true;
        } else if (!std::strcmp(argv[i], "--seq-stats")) {
            cfg.seq_stats = true;
        } else if (!std::strcmp(argv[i], "--flatten")) {
            cfg.flatten = true;
        } else if (!std::strcmp(argv[i], "--gt-stats")) {
            cfg.gt_stats = true;
        } else if (!std::strcmp(argv[i], "--samples") && i + 1 < argc) {
            cfg.samples = argv[++i];
            if (cfg.samples != "struct" && cfg.samples != "long" && cfg.samples != "text") {
                std::fprintf(stderr, "--samples: unknown layout '%s' (use struct|long|text)\n",
                             cfg.samples.c_str());
                std::exit(2);
            }
        } else if (!std::strcmp(argv[i], "--matrix") && i + 1 < argc) {
            cfg.matrix = argv[++i];
            if (cfg.matrix != "wide" && cfg.matrix != "long") {
                std::fprintf(stderr, "--matrix: unknown layout '%s' (use wide|long)\n",
                             cfg.matrix.c_str());
                std::exit(2);
            }
        } else if (!std::strcmp(argv[i], "--distinct")) {
            cfg.distinct = true;
        } else if (!std::strcmp(argv[i], "--box") && i + 1 < argc) {
            cfg.box_style = argv[++i];
        } else if (!std::strcmp(argv[i], "--unique") && i + 1 < argc) {
            cfg.unique_cols = argv[++i];
        } else if (!std::strcmp(argv[i], "--sample") && i + 1 < argc) {
            cfg.sample_n = std::max(0, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) {
            cfg.filter_expr = argv[++i];
        } else if ((!std::strcmp(argv[i], "--select") ||
                    !std::strcmp(argv[i], "--cols")) && i + 1 < argc) {
            cfg.select_cols = argv[++i];
        } else if (!std::strcmp(argv[i], "--json")) {
            cfg.json_array = true;
        } else if (!std::strcmp(argv[i], "--ndjson")) {
            cfg.json_lines = true;
        } else if (!std::strcmp(argv[i], "--md") ||
                   !std::strcmp(argv[i], "--markdown")) {
            cfg.md = true;
        } else if (!std::strcmp(argv[i], "--tree")) {
            cfg.json_tree = true;
        } else if (!std::strcmp(argv[i], "--no-tree")) {
            cfg.json_no_tree = true;
        } else if (!std::strcmp(argv[i], "--pretty")) {
            cfg.json_pretty = true;
        } else if (!std::strcmp(argv[i], "--json-paths")) {
            cfg.json_paths = true;
        } else if (!std::strcmp(argv[i], "--text")) {
            cfg.force_text = true;
        } else if (!std::strcmp(argv[i], "--validate")) {
            cfg.validate = true;
        } else if (!std::strcmp(argv[i], "--decode-pileup")) {
            cfg.decode_pileup = true;
        } else if ((!std::strcmp(argv[i], "--exclude-flags") || !std::strcmp(argv[i], "--ff") ||
                    !std::strcmp(argv[i], "--require-flags") || !std::strcmp(argv[i], "--rf"))
                   && i + 1 < argc) {
            // A flag list: SAM FLAG names (UNMAP,SECONDARY,…) or numbers (0x704).
            const bool excl = argv[i][2] == 'e' || argv[i][2] == 'f';
            pileup_opt = argv[i];
            std::string err;
            int mask = parse_sam_flag_list(argv[++i], &err);
            if (!err.empty()) {
                std::fprintf(stderr, "vv: %s: %s\n", argv[i - 1], err.c_str());
                std::exit(2);
            }
            (excl ? cfg.pileup_excl_flags : cfg.pileup_incl_flags) = mask;
        } else if ((!std::strcmp(argv[i], "--min-mapq") || !std::strcmp(argv[i], "--min-bq"))
                   && i + 1 < argc) {
            const bool mq = argv[i][6] == 'm';
            pileup_opt = argv[i];
            char* end = nullptr;
            long v = std::strtol(argv[++i], &end, 10);
            if (!end || *end || v < 0 || v > 255) {
                std::fprintf(stderr, "vv: %s takes a number 0-255, got '%s'\n",
                             argv[i - 1], argv[i]);
                std::exit(2);
            }
            (mq ? cfg.pileup_min_mapq : cfg.pileup_min_bq) = (int)v;
        } else if (!std::strcmp(argv[i], "--count-orphans")) {
            cfg.pileup_count_orphans = true;
            pileup_opt = argv[i];
        } else if (!std::strcmp(argv[i], "--ignore-overlaps")) {
            cfg.pileup_ignore_overlaps = true;
            pileup_opt = argv[i];
        } else if (!std::strcmp(argv[i], "--pileup")) {
            cfg.pileup = true;
        } else if ((!std::strcmp(argv[i], "-f") ||
                    !std::strcmp(argv[i], "--fasta")) && i + 1 < argc) {
            cfg.pileup_ref = argv[++i];
        } else if (!std::strcmp(argv[i], "--heatmap")) {
            cfg.heatmap = true;
        } else if (!std::strcmp(argv[i], "--image-mode") && i + 1 < argc) {
            cfg.image_mode = argv[++i];
        } else if (!std::strcmp(argv[i], "--tab") && i + 1 < argc) {
            cfg.tab = argv[++i];
        } else if (!std::strcmp(argv[i], "--theme") && i + 1 < argc) {
            cfg.theme = argv[++i];
        } else if (!std::strcmp(argv[i], "--color=auto")) {
            cfg.color = ColorMode::Auto;
        } else if (!std::strcmp(argv[i], "--color=always")) {
            cfg.color = ColorMode::Always;
        } else if (!std::strcmp(argv[i], "--color=never")) {
            cfg.color = ColorMode::Never;
        } else if (!std::strcmp(argv[i], "--color")) {
            // Also accept the space-separated form "--color MODE" (GNU-style);
            // a bare "--color" with no mode (or a non-mode next token, e.g. a
            // filename) means auto.
            if (i + 1 < argc && (!std::strcmp(argv[i + 1], "auto") ||
                                 !std::strcmp(argv[i + 1], "always") ||
                                 !std::strcmp(argv[i + 1], "never"))) {
                const char* m = argv[++i];
                cfg.color = !std::strcmp(m, "always") ? ColorMode::Always
                          : !std::strcmp(m, "never")  ? ColorMode::Never
                                                      : ColorMode::Auto;
            } else {
                cfg.color = ColorMode::Auto;
            }
        } else if (!std::strcmp(argv[i], "--tsv")) {
            cfg.delimiter = '\t';
        } else if (!std::strcmp(argv[i], "--csv")) {
            cfg.delimiter = ',';
        } else if (!std::strcmp(argv[i], "--delimiter") && i + 1 < argc) {
            const char* sep = argv[++i];
            if (!std::strcmp(sep, "tab"))   cfg.delimiter = '\t';
            else if (!std::strcmp(sep, "comma")) cfg.delimiter = ',';
            else if (sep[0] && !sep[1])     cfg.delimiter = sep[0];
            else { std::fprintf(stderr, "delimiter must be a single character\n"); std::exit(1); }
        } else if ((!std::strcmp(argv[i], "--in-delimiter") ||
                    !std::strcmp(argv[i], "-d")) && i + 1 < argc) {
            // Input field separator: read the file as delimited text with this
            // character, overriding the extension (so a space-, semicolon- or
            // pipe-separated file — e.g. R's default write.table() .txt — opens
            // as a table). Distinct from --delimiter, which sets the OUTPUT sep.
            const char* sep = argv[++i];
            if      (!std::strcmp(sep, "tab")   || !std::strcmp(sep, "\\t")) cfg.in_delimiter = '\t';
            else if (!std::strcmp(sep, "space"))     cfg.in_delimiter = ' ';
            else if (!std::strcmp(sep, "comma"))     cfg.in_delimiter = ',';
            else if (!std::strcmp(sep, "semicolon")) cfg.in_delimiter = ';';
            else if (!std::strcmp(sep, "pipe"))      cfg.in_delimiter = '|';
            else if (sep[0] && !sep[1])              cfg.in_delimiter = sep[0];
            else { std::fprintf(stderr, "--in-delimiter must be a single character "
                                        "or one of: tab, space, comma, semicolon, pipe\n");
                   std::exit(1); }
        } else if (!std::strcmp(argv[i], "--header") && i + 1 < argc) {
            // Whether a CSV/TSV first row is a header. auto (default) detects it
            // by comparing row 0 against the types of the rows below; on/off
            // force the choice when the guess is wrong.
            const char* m = argv[++i];
            if      (!std::strcmp(m, "auto"))  cfg.header = HeaderMode::Auto;
            else if (!std::strcmp(m, "on")  || !std::strcmp(m, "yes") ||
                     !std::strcmp(m, "true"))  cfg.header = HeaderMode::On;
            else if (!std::strcmp(m, "off") || !std::strcmp(m, "no")  ||
                     !std::strcmp(m, "false") || !std::strcmp(m, "none"))
                                               cfg.header = HeaderMode::Off;
            else { std::fprintf(stderr, "--header must be one of: auto, on, off\n");
                   std::exit(1); }
        } else if (argv[i][0] != '-' || (argv[i][0] == '-' && argv[i][1] == 0)) {
            // Bare "-" means stdin; treat as a positional argument. Every
            // positional becomes a TUI tab; the first one keeps cfg.path
            // as the "primary" file for non-interactive output modes.
            if (positional++ == 0) cfg.path = argv[i];
            cfg.paths.push_back(argv[i]);
        } else {
            // A known flag that takes an argument falls through to here only
            // when it's the last token (its `i + 1 < argc` guard failed), so
            // report the missing argument specifically rather than the
            // misleading "Unknown option".
            static const std::set<std::string> needs_arg = {
                "-n", "-w", "-c", "-r", "--region", "--window",
                "--regions-file", "--region-cols", "--slop", "--coords",
                "--tail", "--sort", "--tags", "-@", "--threads", "--decode-threads", "--parquet",
                "--arrow", "--feather",
                "--compression", "--unique", "--sample", "--filter",
                "--select", "--cols", "--image-mode", "--tab", "--theme",
                "--expand", "--samples", "--matrix",
                "--delimiter", "--in-delimiter", "-d", "--header",
                "-f", "--fasta", "--box",
                "--exclude-flags", "--ff", "--require-flags", "--rf",
                "--min-mapq", "--min-bq",
            };
            if (needs_arg.count(argv[i])) {
                std::fprintf(stderr, "Option %s requires an argument.\n", argv[i]);
                std::exit(2);
            }
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]); std::exit(1);
        }
    }
    // --formats describes vv itself, so it takes no input file.
    if (cfg.list_formats) return cfg;
    if (cfg.path.empty()) { print_usage(argv[0]); std::exit(1); }
    // NO_COLOR (https://no-color.org): any non-empty value disables colour,
    // unless the user explicitly chose --color=always/never (those win, per the
    // spec). Resolving it into cfg.color here means every downstream consumer
    // (table, delimited, markdown, heatmap, TUI) honours it from one place.
    if (cfg.color == ColorMode::Auto) {
        if (const char* e = std::getenv("NO_COLOR"); e && e[0])
            cfg.color = ColorMode::Never;
    }
    // Validate --image-mode up front so a typo is reported rather than silently
    // ignored (the heatmap renderer also checks, but only when --heatmap runs).
    if (!cfg.image_mode.empty() && cfg.image_mode != "auto" &&
        cfg.image_mode != "kitty" && cfg.image_mode != "iterm" &&
        cfg.image_mode != "sixel" &&
        cfg.image_mode != "halfblock" && cfg.image_mode != "ascii") {
        std::fprintf(stderr, "--image-mode: unknown mode '%s' "
                     "(use auto|kitty|iterm|sixel|halfblock|ascii)\n",
                     cfg.image_mode.c_str());
        std::exit(2);
    }
    // -f/--fasta feeds two things: the reference-aware pileup, and CRAM
    // reference resolution (CRAM stores bases as differences from a
    // reference, so decoding one without $REF_PATH / $REF_CACHE needs it).
    // It adds nothing to a plain BAM read without --pileup, so that stays a
    // usage error rather than a silently ignored flag.
    if (!cfg.pileup_ref.empty() && !cfg.pileup && !fends_ci(cfg.path, ".cram")) {
        std::fprintf(stderr, "-f/--fasta applies to --pileup "
                     "(reference-aware pileup) or to a CRAM input "
                     "(reference resolution)\n");
        std::exit(2);
    }
    // The read / base filters shape --pileup's rows; on any other view they
    // would be silently ignored.
    if (pileup_opt && !cfg.pileup) {
        std::fprintf(stderr, "vv: %s applies to --pileup\n", pileup_opt);
        std::exit(2);
    }
    return cfg;
}
