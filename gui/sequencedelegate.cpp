#include "sequencedelegate.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QTableView>

#include <algorithm>

SequenceDelegate::SequenceDelegate(Kind kind, QTableView* view)
    : QStyledItemDelegate(view), kind_(kind), view_(view) {}

QColor SequenceDelegate::baseColor(QChar c, bool dark) {
    QColor col;
    switch (c.toUpper().unicode()) {
    case 'A': col = QColor(0x1a, 0x9a, 0x3a); break;
    case 'C': col = QColor(0x24, 0x63, 0xd1); break;
    case 'G': col = QColor(0xd1, 0x84, 0x00); break;
    case 'T': case 'U': col = QColor(0xd0, 0x2c, 0x2c); break;
    case 'N': col = QColor(0x8a, 0x8a, 0x8a); break;
    default: return {};
    }
    return dark ? col.lighter(150) : col;
}

QColor SequenceDelegate::qualityColor(int q) {
    const double t = std::clamp(q, 0, 40) / 40.0;    // red -> yellow -> green
    QColor c = QColor::fromHsvF(t * 120.0 / 360.0, 0.55, 0.95);
    c.setAlpha(110);
    return c;
}

void SequenceDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const {
    QStyleOptionViewItem o = opt;
    initStyleOption(&o, idx);
    const QString text = o.text;
    // A cell not read yet ("…"), or empty: as any other cell.
    if (text.isEmpty() || text == QStringLiteral("…")) {
        QStyledItemDelegate::paint(p, opt, idx);
        return;
    }
    o.text.clear();
    const QWidget* w = o.widget;
    QStyle* style = w ? w->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &o, p, w);
    if (view_ && idx.column() == view_->currentIndex().column()) {
        QColor c = o.palette.color(QPalette::Highlight);
        c.setAlpha(40);
        p->fillRect(o.rect, c);
    }
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(o.font.pointSizeF() > 0 ? o.font.pointSizeF() : font.pointSizeF());
    const QFontMetrics fm(font);
    const int cw = std::max(1, fm.horizontalAdvance(QLatin1Char('A')));
    const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, w) + 1;
    const QRect r = o.rect.adjusted(margin, 0, -margin, 0);
    const int n = std::min<int>(text.size(), std::max(0, r.width() / cw));
    const bool selected = o.state & QStyle::State_Selected;
    const bool dark = o.palette.color(QPalette::Base).lightness() < 128;
    const QColor plain = o.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
    p->save();
    p->setClipRect(o.rect);
    p->setFont(font);
    for (int i = 0; i < n; ++i) {
        const QRect cell(r.left() + i * cw, r.top(), cw, r.height());
        const QChar ch = text.at(i);
        QColor pen = plain;
        if (kind_ == Kind::Quality) {
            if (!selected) p->fillRect(cell.adjusted(0, 2, 0, -2), qualityColor(ch.unicode() - 33));
        } else if (!selected) {
            if (const QColor b = baseColor(ch, dark); b.isValid()) pen = b;
        }
        p->setPen(pen);
        p->drawText(cell, Qt::AlignCenter, QString(ch));
    }
    if (n < text.size() && n > 0) {   // more than fits: an ellipsis over the last cell
        const QRect last(r.left() + (n - 1) * cw, r.top(), cw, r.height());
        const QPalette::ColorRole bg = selected ? QPalette::Highlight
                                     : (o.features & QStyleOptionViewItem::Alternate) ? QPalette::AlternateBase
                                                                                       : QPalette::Base;
        p->fillRect(last, o.palette.brush(bg));
        p->setPen(plain);
        p->drawText(last, Qt::AlignCenter, QStringLiteral("…"));
    }
    p->restore();
}
