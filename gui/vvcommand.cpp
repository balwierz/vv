#include "vvcommand.h"

// Both are the core's (view_command_line, shell_quote), so vvg's command and
// the viewer's `Y` stay the same for the same view.
QString shellQuote(const QString& word) {
    return QString::fromStdString(shell_quote(word.toStdString()));
}

QString vvCommandLine(const VvCommandSpec& s) {
    return QString::fromStdString(view_command_line(vvCommandConfig(s)));
}

Config vvCommandConfig(const VvCommandSpec& s) {
    Config cfg;
    cfg.path = s.path.toStdString();
    cfg.tab  = s.tab.toStdString();
    if (s.contigs) {
        cfg.contigs = true;
    } else {
        if (!s.region.isEmpty()) {
            cfg.region           = s.region.toStdString();
            cfg.coords_one_based = s.ncbi;
            cfg.slop             = s.slop;
            cfg.index_path       = s.index.toStdString();
        }
        cfg.pileup_ref = s.reference.toStdString();
        cfg.pileup   = s.pileup;
        cfg.bam_tags = s.tags.toStdString();
        cfg.gt_stats = s.gtStats;
        cfg.expand_col = s.expand.toStdString();
    }
    if (!s.samples.isEmpty()) cfg.samples = s.samples.toStdString();
    if (!s.matrix.isEmpty())  cfg.matrix  = s.matrix.toStdString();
    if (!s.types.isEmpty()) (void)parse_col_types(s.types.toStdString(), &cfg.col_types);
    cfg.filter_expr = s.filter.toStdString();
    for (const auto& [col, desc] : s.sort) {
        if (cfg.sort_col.empty()) { cfg.sort_col = col.toStdString(); cfg.sort_desc = desc; }
        else cfg.sort_then.push_back({col.toStdString(), desc});
    }
    cfg.select_cols = s.select.join(QLatin1Char(',')).toStdString();
    cfg.describe    = s.describe;
    cfg.row_subset  = s.rows;
    return cfg;
}
