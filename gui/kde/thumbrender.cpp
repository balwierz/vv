#include "thumbrender.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>

#include <memory>
#include <vector>

#include "vv/vvcore.hpp"

namespace {
QString cell_text(const std::shared_ptr<arrow::Table>& tbl, int col, int64_t row) {
    auto ca = tbl->column(col);
    int64_t i = row;
    for (const auto& a : ca->chunks()) {
        if (i < a->length()) return QString::fromStdString(cell_to_display_string(*a, i));
        i -= a->length();
    }
    return {};
}
}  // namespace

// Runs inside Dolphin's thumbnail worker on whatever files the user browses,
// so a malformed/corrupt input must never take the process down. The
// function-try-block turns any C++ exception (std::bad_alloc, an Arrow/htslib
// throw) into "no thumbnail". (Note: a libvvcore .ValueOrDie() on an error
// Status would std::abort and is *not* catchable — the source readers validate
// untrusted input up front to avoid reaching those.)
QImage vv_render_thumbnail(const QString& path, const QSize& target) try {
    Config cfg;
    cfg.path = path.toStdString();
    std::unique_ptr<TabularSource> src;
    if (!open_source(cfg.path, cfg, &src).empty() || !src) return {};

    auto schema = src->schema();
    if (!schema) return {};
    const int ncols = std::min(6, schema->num_fields());
    if (ncols <= 0) return {};
    std::vector<int> cols;
    for (int i = 0; i < ncols; ++i) cols.push_back(i);

    const int wantRows = 14;
    std::shared_ptr<arrow::Table> tbl;
    if (!src->read_first(wantRows, cols, &tbl).ok() || !tbl) return {};
    const int rows = (int)std::min<int64_t>(wantRows, tbl->num_rows());

    QSize sz = (target.isValid() && target.width() >= 32 && target.height() >= 32)
                   ? target : QSize(256, 256);
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);

    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int totalRows = rows + 1;                 // +1 header
    qreal cellH = std::max<qreal>(10.0, (qreal)sz.height() / std::max(1, totalRows));
    qreal colW  = (qreal)sz.width() / ncols;

    int fpx = std::max(6, std::min(13, (int)(cellH * 0.62)));
    QFont f("monospace", -1);
    f.setPixelSize(fpx);
    p.setFont(f);
    QFontMetrics fm(f);

    auto drawText = [&](const QString& s, qreal x, qreal y, qreal w, bool bold) {
        f.setBold(bold);
        p.setFont(f);
        QString t = fm.elidedText(s, Qt::ElideRight, (int)(w - 6));
        p.drawText(QRectF(x + 3, y, w - 6, cellH),
                   Qt::AlignVCenter | Qt::AlignLeft, t);
    };

    // Header band.
    p.fillRect(QRectF(0, 0, sz.width(), cellH), QColor(45, 108, 223));
    p.setPen(Qt::white);
    for (int c = 0; c < ncols; ++c)
        drawText(QString::fromStdString(schema->field(c)->name()),
                 c * colW, 0, colW, /*bold=*/true);

    // Data rows (zebra).
    for (int r = 0; r < rows; ++r) {
        qreal y = cellH * (r + 1);
        if (r % 2)
            p.fillRect(QRectF(0, y, sz.width(), cellH), QColor(244, 247, 252));
        p.setPen(QColor(40, 40, 40));
        for (int c = 0; c < ncols; ++c)
            drawText(cell_text(tbl, c, r), c * colW, y, colW, /*bold=*/false);
    }

    // Column separators.
    p.setPen(QColor(225, 228, 235));
    for (int c = 1; c < ncols; ++c)
        p.drawLine(QPointF(c * colW, 0), QPointF(c * colW, sz.height()));

    p.end();
    return img;
} catch (...) {
    return {};   // corrupt / unsupported input → no thumbnail, never abort
}
