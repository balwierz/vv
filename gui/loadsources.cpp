#include "loadsources.h"

LoadResult loadSources(const Config& base, const QStringList& paths, bool expand) {
    LoadResult r;
    for (const QString& p : paths) {
        Config cfg = base;
        cfg.path = p.toStdString();
        if (expand && !cfg.contigs && cfg.expand_col.empty())
            cfg.expand_col = packed_column_for(cfg.path);
        std::unique_ptr<TabularSource> src;
        std::string err = open_source(cfg.path, cfg, &src);
        if (!err.empty() || !src) {
            r.errors << QStringLiteral("%1: %2").arg(
                p, QString::fromStdString(err.empty() ? "open failed" : err));
            continue;
        }
        auto siblings = src->expand_tabs();
        auto owner = std::make_shared<SourceOwner>();
        r.sources.push_back(std::move(src));
        const QString ex = QString::fromStdString(cfg.expand_col);
        r.origins.push_back(p);
        r.expands.push_back(ex);
        r.primary.push_back(true);
        r.owners.push_back(owner);
        for (auto& s : siblings) {
            r.sources.push_back(std::move(s));
            r.origins.push_back(p);
            r.expands.push_back(ex);
            r.primary.push_back(false);
            r.owners.push_back(owner);
        }
        r.opened << p;
    }
    return r;
}

