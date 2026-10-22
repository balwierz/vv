// file_features / tab_traits: which of vv's format options a file and a tab
// offer, for a viewer that shows only the controls that apply (vvg).
#include "internal.hpp"

#include <filesystem>

namespace {

// `path` without its last extension (`x.bam` -> `x`), for `x.bai`.
std::string drop_ext(const std::string& path) {
    const size_t dot = path.rfind('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
    return path.substr(0, dot);
}

}  // namespace

FileFeatures file_features(const std::string& path) {
    FileFeatures f;
    // The extension as open_source() reads it: .zst / .zstd / .bz2 / .xz are
    // compression wrappers, .bgz is .gz (dispatch_by_kind in open.cpp).
    std::string det = path;
    for (const char* z : {".zstd", ".zst", ".bz2", ".xz"})
        if (fends_ci(det, z)) { det.resize(det.size() - std::strlen(z)); break; }
    if (fends_ci(det, ".bgz")) det = det.substr(0, det.size() - 4) + ".gz";
    auto ext = [&](std::initializer_list<const char*> exts, bool gz = true) {
        for (const char* e : exts)
            if (fends_ci(det, e) || (gz && fends_ci(det, std::string(e) + ".gz"))) return true;
        return false;
    };
    const bool remote = is_remote_url(path);
    std::error_code ec;
    if (!remote && std::filesystem::is_directory(path, ec)) return f;   // a dataset or a Zarr store
    // An index file beside `path`; a remote file is taken to have one (the
    // query reports it when not).
    auto beside = [&](std::initializer_list<std::string> names) {
        if (remote) return true;
        for (const auto& n : names)
            if (std::filesystem::exists(n, ec)) return true;
        return false;
    };
    const std::string stem = drop_ext(path);
    const std::string quoted = "'" + path + "'";

    const bool bam  = fends_ci(path, ".bam");
    const bool cram = fends_ci(path, ".cram");
    const bool sam  = ext({".sam"});
    const bool bcf  = fends_ci(path, ".bcf");
    const bool vcf  = ext({".vcf"});
    const bool paf  = ext({".paf"});
    const bool gff  = ext({".gff", ".gff3", ".gtf"});
    const bool bed  = ext({".bed", ".narrowPeak", ".broadPeak", ".gappedPeak", ".bedGraph", ".bg",
                           ".tagAlign"}) && !(fends_ci(det, ".bed") && is_plink_bed(path));
    const bool pile = ext({".pileup", ".mpileup", ".pile"});
    const bool tsv  = ext({".tsv", ".csv"});
    const bool fa   = fastx_ext(path, det, {".fa", ".fasta", ".fna", ".faa", ".ffn", ".frn"});

    if (bam || cram) {
        if (beside({path + ".bai", stem + ".bai", path + ".csi", path + ".crai", stem + ".crai"}))
            f.region = FileFeatures::Region::Ready;
        else {
            f.region = FileFeatures::Region::NeedsIndex;
            f.region_hint = "a region needs an index: samtools index " + quoted;
        }
    } else if (bcf) {
        if (beside({path + ".csi", stem + ".csi", path + ".tbi"})) f.region = FileFeatures::Region::Ready;
        else {
            f.region = FileFeatures::Region::NeedsIndex;
            f.region_hint = "a region needs an index: bcftools index " + quoted;
        }
    } else if (vcf || gff || bed || sam || paf || pile) {
        // Without a tabix index the reader scans the whole file for the
        // region (region.cpp RegionScanStream).
        f.region = FileFeatures::Region::Ready;
        if (!beside({path + ".tbi", path + ".csi"}))
            f.region_hint = "no tabix index: a region reads the whole file";
    } else if (tsv) {
        // Only a table someone indexed by its coordinates (bgzip + tabix -s
        // -b -e): most CSV / TSV files have none to query.
        if (beside({path + ".tbi", path + ".csi"})) f.region = FileFeatures::Region::Ready;
    } else if (fa) {
        if (beside({path + ".fai"})) f.region = FileFeatures::Region::Ready;
        else {
            f.region = FileFeatures::Region::NeedsIndex;
            f.region_hint = "a region needs an index: samtools faidx " + quoted;
        }
    } else if (ext({".bb", ".bigBed", ".bw", ".bigWig", ".2bit"}, false) || ext({".wig"}) ||
               ext({".cool", ".mcool", ".scool", ".lociss"}, false) ||
               ext({".gb", ".gbk", ".gbff", ".genbank", ".embl"})) {
        f.region = FileFeatures::Region::Ready;
    } else if (ext({".parquet", ".pq"}, false)) {
        f.region_by_columns = true;
    }

    f.pileup = bam || cram;
    f.tags = bam || cram || sam || paf;
    f.variants = vcf || bcf;
    if (bam || cram || sam || vcf || bcf) {
        // Header only, quietly: a header warning belongs to the file's own
        // open, not to this probe.
        GenomicHeader h;
        const int level = hts_get_log_level();
        hts_set_log_level(HTS_LOG_OFF);
        const std::string err = read_genomic_header(path, "", &h);
        hts_set_log_level((htsLogLevel)level);
        if (err.empty()) {
            f.contigs = !h.contig_names.empty();
            f.has_samples = f.variants && !h.samples.empty();
        }
    }
    f.expand_column = packed_column_for(path);
    return f;
}

TabTraits tab_traits(const TabularSource& src) {
    TabTraits t;
    t.matrix_tab = src.is_matrix_tab();
    t.slices = src.has_slices();
    t.cells_in_full = src.show_cells_in_full();
    if (auto sch = src.schema()) t.coord_columns = has_genomic_coord_columns(*sch);
    return t;
}
