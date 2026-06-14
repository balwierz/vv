#include "metaprobe.h"

#include <QStringList>
#include <algorithm>
#include <memory>
#include "vv/vvcore.hpp"

// Runs inside the Baloo / KFileMetaData extractor on browsed files, so a
// malformed input must never abort the worker. The function-try-block maps any
// C++ exception to "not ok" (VvMeta defaults ok=false). (A .ValueOrDie() abort
// in libvvcore is not catchable; the source readers validate untrusted input.)
VvMeta vv_probe_meta(const QString& path) try {
    VvMeta m;
    Config cfg;
    cfg.path = path.toStdString();
    std::unique_ptr<TabularSource> src;
    if (!open_source(cfg.path, cfg, &src).empty() || !src) return m;

    auto schema = src->schema();
    if (!schema) return m;

    m.ok        = true;
    m.rows      = src->total_rows();             // -1 if streaming / unknown
    m.footer    = QString::fromStdString(src->footer());
    m.generator = QString::fromStdString(src->created_by());
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
} catch (...) {
    return VvMeta{};   // ok=false: corrupt / unsupported input, never abort
}
