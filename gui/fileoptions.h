// FileOptions — the format options of one file open in vvg.
#pragma once

#include "vv/vvcore.hpp"

#include <QJsonObject>
#include <QString>

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
    // --index: the file's index when it is not beside it (one vvg built in
    // its cache). Looked up when the file opens, not kept in the session.
    std::string index;
    bool operator==(const FileOptions&) const = default;

    // For the session (vvg's settings).
    QJsonObject toJson() const {
        auto q = [](const std::string& s) { return QString::fromStdString(s); };
        return QJsonObject{{QStringLiteral("region"), q(region)}, {QStringLiteral("ncbi"), ncbi},
                           {QStringLiteral("slop"), (double)slop}, {QStringLiteral("pileup"), pileup},
                           {QStringLiteral("tags"), q(tags)}, {QStringLiteral("gtStats"), gtStats},
                           {QStringLiteral("contigs"), contigs}, {QStringLiteral("expand"), expand},
                           {QStringLiteral("samples"), q(samples)}, {QStringLiteral("matrix"), q(matrix)}};
    }
    static FileOptions fromJson(const QJsonObject& o) {
        FileOptions f;
        auto str = [&](const char* k, const std::string& def) {
            const QJsonValue v = o.value(QLatin1String(k));
            return v.isString() ? v.toString().toStdString() : def;
        };
        f.region  = str("region", f.region);
        f.ncbi    = o.value(QStringLiteral("ncbi")).toBool(f.ncbi);
        f.slop    = (int64_t)o.value(QStringLiteral("slop")).toDouble((double)f.slop);
        f.pileup  = o.value(QStringLiteral("pileup")).toBool(f.pileup);
        f.tags    = str("tags", f.tags);
        f.gtStats = o.value(QStringLiteral("gtStats")).toBool(f.gtStats);
        f.contigs = o.value(QStringLiteral("contigs")).toBool(f.contigs);
        f.expand  = o.value(QStringLiteral("expand")).toBool(f.expand);
        f.samples = str("samples", f.samples);
        f.matrix  = str("matrix", f.matrix);
        return f;
    }

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
        c.index_path       = index;
        return c;
    }
};
