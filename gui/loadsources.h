// loadSources — open files through libvvcore and flatten each file's tabs
// (vvg, the KDE part and the Dolphin Properties page).
#pragma once

#include "sourceowner.h"
#include "vv/vvcore.hpp"

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

// Open every path through libvvcore (using a shared base Config so a later
// region/pileup query applies uniformly) and flatten each file's sibling tabs
// — sheets / tables / datasets / NPZ arrays — into one source list. Per-path
// open errors are collected rather than thrown; `opened` lists the paths that
// produced at least one source (for the recent-files list).
struct LoadResult {
    std::vector<std::unique_ptr<TabularSource>> sources;
    std::vector<QString>                        origins;   // per source: the path it came from
    std::vector<QString>                        expands;   // per source: its --expand column
    std::vector<bool>                           primary;   // per source: first tab of its file
    // per source: the thread its file's tabs are read on (shared by siblings)
    std::vector<std::shared_ptr<SourceOwner>>   owners;
    QStringList                                 opened;
    QStringList                                 errors;
};
// `expand`: unpack each file's packed key=value column (GFF/GTF attributes,
// VCF/BCF INFO — see packed_column_for) into real columns, as --expand does.
LoadResult loadSources(const Config& base, const QStringList& paths, bool expand = false);
