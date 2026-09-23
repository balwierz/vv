#include "vvcommand.h"

QString shellQuote(const QString& word) {
    if (word.isEmpty()) return QStringLiteral("''");
    bool plain = true;
    for (QChar c : word) {
        const ushort u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                        (u >= '0' && u <= '9') ||
                        QStringLiteral("_-.,/:=@%+").contains(c);
        if (!ok) { plain = false; break; }
    }
    if (plain) return word;
    QString q = word;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QString vvCommandLine(const VvCommandSpec& s) {
    QStringList w{QStringLiteral("vv")};
    auto opt = [&](const char* flag, const QString& value) {
        w << QString::fromLatin1(flag) << shellQuote(value);
    };
    if (!s.tab.isEmpty()) opt("--tab", s.tab);
    if (s.contigs) {
        w << QStringLiteral("--contigs");
    } else {
        if (!s.region.isEmpty()) {
            opt("-r", s.region);
            if (s.ncbi)     opt("--coords", QStringLiteral("NCBI"));
            if (s.slop > 0) opt("--slop", QString::number(s.slop));
        }
        if (s.pileup)            w << QStringLiteral("--pileup");
        if (!s.tags.isEmpty())   opt("--tags", s.tags);
        if (s.gtStats)           w << QStringLiteral("--gt-stats");
        if (!s.expand.isEmpty()) opt("--expand", s.expand);
    }
    if (!s.filter.isEmpty())     opt("--filter", s.filter);
    // --sort reads a trailing :asc / :desc as the direction, so a name that
    // contains a colon always gets an explicit one.
    if (!s.sortColumn.isEmpty())
        opt("--sort", s.sortColumn + (s.sortDesc ? QStringLiteral(":desc")
                                      : s.sortColumn.contains(QLatin1Char(':'))
                                            ? QStringLiteral(":asc") : QString()));
    if (!s.select.isEmpty())     opt("--select", s.select.join(QLatin1Char(',')));
    w << shellQuote(s.path);
    return w.join(QLatin1Char(' '));
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
        }
        cfg.pileup   = s.pileup;
        cfg.bam_tags = s.tags.toStdString();
        cfg.gt_stats = s.gtStats;
        cfg.expand_col = s.expand.toStdString();
    }
    cfg.filter_expr = s.filter.toStdString();
    cfg.sort_col    = s.sortColumn.toStdString();
    cfg.sort_desc   = s.sortDesc;
    cfg.select_cols = s.select.join(QLatin1Char(',')).toStdString();
    return cfg;
}
