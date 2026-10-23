// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// The format registry: what vv reads (format_table), shared by `vv --formats`
// and vvg's Open dialog.
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

#include "internal.hpp"

// Families: the underlying format, one entry each in vvg's Open dialog.
static constexpr const char* kColumnar     = "Columnar (Parquet, Arrow, ORC, POD5)";
static constexpr const char* kAlignments   = "Alignments (BAM, CRAM, SAM, PAF, pileup)";
static constexpr const char* kVariants     = "Variants (VCF, BCF, PLINK)";
static constexpr const char* kIntervals    = "Intervals and annotations (BED, GFF, bigBed, bigWig)";
static constexpr const char* kDelimited    = "Delimited text (TSV, CSV, BLAST, MatrixMarket)";
static constexpr const char* kSequences    = "Sequences (FASTA, FASTQ, 2bit, GenBank)";
static constexpr const char* kHdf5         = "HDF5 (AnnData, Loom, MuData, Cooler)";
static constexpr const char* kArrays       = "Arrays (Zarr, NumPy)";
static constexpr const char* kRdata        = "R data (rds, RData)";
static constexpr const char* kDatabases    = "Databases (SQLite, DuckDB)";
static constexpr const char* kSpreadsheets = "Spreadsheets (xlsx, ods)";
static constexpr const char* kJson         = "JSON (JSON, NDJSON)";
static constexpr const char* kText         = "Text (Markdown, plain text)";

static const FormatInfo kFormats[] = {
  {"Apache Parquet", ".parquet", "ParquetSource",
   false, true,  false, false, true,  "needs chrom/start/end columns (--region-cols)", kColumnar},
  {"LociSSD", ".lociss", "LocissV4Source / ParquetSource",
   false, true,  false, false, true,  "v3 via row-group stats, v4 via the zone map", kColumnar},
  {"Arrow IPC", ".arrow", "IpcSource",
   false, false, false, false, true,  "", kColumnar},
  {"Feather", ".feather", "IpcSource",
   false, false, false, false, true,  "", kColumnar},
  {"Apache ORC", ".orc", "OrcSource",
   false, false, false, false, false, "", kColumnar},
  {"BAM / CRAM alignments", ".bam .cram", "BamSource / BamPileupSource",
   false, true,  false, true,  false, "needs a .bai/.csi/.crai index", kAlignments},
  {"SAM alignments (text)", ".sam", "DelimitedSource",
   false, false, false, true,  false, "no index; convert with `samtools view -b`", kAlignments},
  {"BCF", ".bcf", "BcfSource",
   false, true,  false, true,  false, "needs a .csi/.tbi index", kVariants},
  {"VCF", ".vcf", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix", kVariants},
  {"GFF / GFF3 / GTF", ".gff .gff3 .gtf", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix", kIntervals},
  {"BED", ".bed", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix", kIntervals},
  {"ENCODE peak / signal", ".narrowPeak .broadPeak .gappedPeak .bedGraph .bg .tagAlign",
   "DelimitedSource", true, true, false, true, false, "bgzip + tabix", kIntervals},
  {"Delimited text", ".tsv .csv", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix", kDelimited},
  {"samtools mpileup", ".pileup .mpileup .pile", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix", kAlignments},
  {"PAF (minimap2)", ".paf", "DelimitedSource",
   true,  false, false, true,  false, "", kAlignments},
  {"Arrow IPC stream", ".arrows", "IpcStreamSource",
   false, false, false, true,  true,  "", kColumnar},
  {"MatrixMarket (sparse matrix)", ".mtx", "DelimitedSource",
   true,  false, false, true,  false, "", kDelimited},
  {"PLINK variant / sample tables", ".bim .fam .pvar .psam", "DelimitedSource",
   true,  false, false, true,  false, "", kVariants},
  {"R data", ".rds .RData .rda", "RdataSource",
   false, false, true,  false, false, "", kRdata},
  {"UCSC wiggle", ".wig", "DelimitedSource",
   true,  false, false, true,  false, "", kIntervals},
  {"BLAST / DIAMOND tabular", ".m8 .blast6 .outfmt6", "DelimitedSource",
   true,  false, false, true,  false, "", kDelimited},
  {"Genomics TSV layouts", ".bedpe .pairs .gct .maf", "DelimitedSource",
   true,  false, false, true,  false, "", kIntervals},
  {"FASTA", ".fa .fasta .fna .faa .ffn .frn", "FastxSource",
   true,  true,  false, true,  false, "needs a .fai (bgzip + .gzi for .fa.gz); one row per window", kSequences},
  {"FASTQ", ".fq .fastq", "FastxSource",
   true,  false, false, true,  false, "", kSequences},
  {"UCSC bigBed / bigWig", ".bb .bigBed .bw .bigWig", "BigSource",
   false, true,  false, true,  false, "native block-level overlap; no sidecar index", kIntervals},
  {"UCSC 2bit", ".2bit", "TwoBitSource",
   false, false, false, false, false, "", kSequences},
  {"DuckDB", ".duckdb .ddb", "DuckSource",
   false, false, true,  false, true,  "", kDatabases},
  {"SQLite", ".sqlite .sqlite3 .db", "SqliteSource",
   false, false, true,  true,  false, "", kDatabases},
  {"Excel workbook", ".xlsx .xlsm", "XlsxSource",
   false, false, true,  false, false, "", kSpreadsheets},
  {"OpenDocument spreadsheet", ".ods .fods", "OdsSource",
   false, false, true,  false, false, "", kSpreadsheets},
  {"HDF5 / AnnData / Loom", ".h5ad .h5 .hdf5 .loom .h5mu .h5seurat .fast5 .nc .nc4", "Hdf5Source",
   false, false, true,  false, false, "", kHdf5},
  {"GenBank / EMBL", ".gb .gbk .gbff .genbank .embl", "FlatFileSource",
   true,  true,  true,  false, false, "", kSequences},
  {"Nanopore POD5", ".pod5", "Pod5Tab",
   false, false, true,  true,  true,  "", kColumnar},
  {"Zarr store", ".zarr .zarr.zip", "ZarrStore",
   false, false, true,  false, false, "", kArrays},
  {"Cooler Hi-C matrix", ".cool .mcool .scool", "PixelsSource",
   false, true,  true,  true,  false, "", kHdf5},
  {"NumPy archive", ".npz", "NpzSource",
   false, false, true,  false, false, "", kArrays},
  {"NumPy array", ".npy", "NpzSource",
   false, false, false, false, false, "", kArrays},
  {"Markdown", ".md .markdown .mdown .mkd", "md4c renderer",
   false, false, false, false, false, "", kText},
  {"JSON / NDJSON", ".json .ndjson .jsonl .geojson .ipynb .har", "JsonSource",
   true,  false, false, true,  false, "", kJson},
  // Plain text is last on purpose: it is the fallback, and any file no other
  // row claims is content-sniffed into it. The extension list is short by
  // design — .py / .c / .conf / .toml reach the same reader through the
  // sniff, and claiming them here would advertise vv as a code viewer.
  {"Plain text", ".txt .text .log", "TextSource",
   true,  false, false, true,  true,  "", kText},
};

const std::vector<FormatInfo>& format_table() {
    static const std::vector<FormatInfo> table(std::begin(kFormats), std::end(kFormats));
    return table;
}

std::vector<std::string> format_extensions(const FormatInfo& f) {
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

std::vector<std::string> format_globs(const FormatInfo& f) {
    // open_source() looks through .zst / .zstd / .bz2 / .xz and reads .bgz as
    // .gz (dispatch_by_kind), for the formats whose gz flag is set.
    static const char* const kWrappers[] = {".gz", ".bgz", ".zst", ".bz2", ".xz"};
    std::vector<std::string> out;
    auto add = [&](const std::string& ext) {
        out.push_back("*" + ext);
        if (f.gz)
            for (const char* w : kWrappers) out.push_back("*" + ext + w);
    };
    for (const std::string& ext : format_extensions(f)) {
        add(ext);
        std::string lower = ext;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower != ext) add(lower);
    }
    return out;
}

const char* vv_version() { return kVersion; }
const char* vv_doi() { return kDoi; }
