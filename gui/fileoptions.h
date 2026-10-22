// FileOptions — the format options of one file open in vvg.
#pragma once

#include "vv/vvcore.hpp"

#include <string>

// The format options of one open file — vv's --region / --coords / --slop /
// --pileup / --tags / --gt-stats / --contigs / --expand / --samples /
// --matrix. Each file has its own: a change re-opens only that file's tabs.
struct FileOptions {
    std::string region;
    bool        ncbi = false;           // the region is NCBI 1-based
    int64_t     slop = 0;
    bool        pileup = false;
    std::string tags;
    bool        gtStats = false;
    bool        contigs = false;
    bool        expand = true;          // unpack GFF/GTF attributes, VCF/BCF INFO
    std::string samples = "struct";
    std::string matrix = "wide";
    bool operator==(const FileOptions&) const = default;

    Config config() const {
        Config c;
        c.region           = region;
        c.coords_one_based = ncbi;
        c.slop             = slop;
        c.pileup           = pileup;
        c.bam_tags         = tags;
        c.gt_stats         = gtStats;
        c.contigs          = contigs;
        c.samples          = samples;
        c.matrix           = matrix;
        return c;
    }
};
