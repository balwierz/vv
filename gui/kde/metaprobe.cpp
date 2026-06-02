#include "metaprobe.h"

#include <QStringList>
#include <algorithm>
#include <memory>
#include "vv/vvcore.hpp"

VvMeta vv_probe_meta(const QString& path) {
    VvMeta m;
    Config cfg;
    cfg.path = path.toStdString();
    std::unique_ptr<TabularSource> src;
    if (!open_source(cfg.path, cfg, &src).empty() || !src) return m;

    m.ok        = true;
    m.rows      = src->total_rows();             // -1 if streaming / unknown
    m.footer    = QString::fromStdString(src->footer());
    m.generator = QString::fromStdString(src->created_by());

    auto schema = src->schema();
    m.cols = schema->num_fields();
    const int show = std::min(12, m.cols);
    QStringList parts;
    for (int i = 0; i < show; ++i) {
        const auto& f = schema->field(i);
        parts << QString::fromStdString(f->name() + ":" + f->type()->ToString());
    }
    if (m.cols > show) parts << QStringLiteral("…");
    m.schema = parts.join(QStringLiteral(", "));
    return m;
}
