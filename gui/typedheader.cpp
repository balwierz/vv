#include "typedheader.h"

#include <QPainter>
#include <QStyleOptionHeader>

TypedHeaderView::TypedHeaderView(QWidget* parent) : QHeaderView(Qt::Horizontal, parent) {}

void TypedHeaderView::setRuler(int logicalIndex, int advance, int inset) {
    if (advance > 0) rulers_.insert(logicalIndex, {advance, inset});
    else rulers_.remove(logicalIndex);
    viewport()->update();
}

QList<QPair<int, int>> TypedHeaderView::rulerMarks(int logicalIndex, int width) const {
    QList<QPair<int, int>> marks;
    const auto it = rulers_.constFind(logicalIndex);
    if (it == rulers_.constEnd()) return marks;
    const auto [advance, inset] = *it;
    const int labelW = QFontMetrics(typeFont()).horizontalAdvance(QStringLiteral("0000"));
    int lastEnd = -labelW;
    for (int pos = 1; inset + (pos - 1) * advance < width; pos = pos == 1 ? 10 : pos + 10) {
        const int x = inset + (pos - 1) * advance;
        if (x < lastEnd) continue;                 // no room since the last label
        marks.append({pos, x});
        lastEnd = x + QFontMetrics(typeFont()).horizontalAdvance(QString::number(pos)) + advance;
    }
    return marks;
}

void TypedHeaderView::setShowTypes(bool on) {
    if (on == show_) return;
    show_ = on;
    // The header's height follows its contents: re-measure, repaint.
    if (count() > 0) headerDataChanged(orientation(), 0, count() - 1);
    updateGeometry();
    viewport()->update();
}

QFont TypedHeaderView::typeFont() const {
    QFont f = font();
    if (f.pointSizeF() > 0) f.setPointSizeF(f.pointSizeF() * 0.85);
    else f.setPixelSize(std::max(1, (int)(f.pixelSize() * 0.85)));
    f.setItalic(true);
    return f;
}

QString TypedHeaderView::typeOf(int logicalIndex) const {
    if (!show_ || !model()) return {};
    return model()->headerData(logicalIndex, orientation(), TypeRole).toString();
}

void TypedHeaderView::paintSection(QPainter* p, const QRect& rect, int logicalIndex) const {
    const QString type = typeOf(logicalIndex);
    if (type.isEmpty() || !rect.isValid()) {
        QHeaderView::paintSection(p, rect, logicalIndex);
        paintSortMark(p, rect, logicalIndex);
        return;
    }
    QStyleOptionHeader opt;
    initStyleOption(&opt);
    initStyleOptionForIndex(&opt, logicalIndex);
    opt.rect = rect;
    // The section (background, frame, sort indicator) without its label.
    QStyleOptionHeader section = opt;
    section.text.clear();
    section.icon = QIcon();
    style()->drawControl(QStyle::CE_Header, &section, p, this);

    // The name, then the type under it, centred together in the label area.
    const QRect label = style()->subElementRect(QStyle::SE_HeaderLabel, &opt, this);
    const QFont tf = typeFont();
    const QFontMetrics tfm(tf);
    const int nameH = opt.fontMetrics.height(), typeH = tfm.height();
    const int top = label.top() + std::max(0, (label.height() - nameH - typeH) / 2);
    QStyleOptionHeader name = opt;
    name.rect = QRect(label.left(), top, label.width(), nameH);
    style()->drawControl(QStyle::CE_HeaderLabel, &name, p, this);

    const QRect typeRect(label.left(), top + nameH, label.width(), typeH);
    p->save();
    p->setFont(tf);
    p->setPen(opt.palette.color(QPalette::PlaceholderText));
    if (rulers_.contains(logicalIndex)) {
        // Positions over the characters: a tick and the number after it.
        p->setClipRect(rect);
        QFont rf = tf;
        rf.setItalic(false);
        p->setFont(rf);
        for (const auto& [pos, x] : rulerMarks(logicalIndex, rect.width())) {
            const int left = rect.left() + x;
            p->drawLine(left, typeRect.top() + 2, left, typeRect.bottom());
            p->drawText(QRect(left + 2, typeRect.top(), 60, typeH), Qt::AlignLeft | Qt::AlignVCenter,
                        QString::number(pos));
        }
    } else {
        const Qt::Alignment h = opt.textAlignment & Qt::AlignHorizontal_Mask;
        p->drawText(typeRect, int(h | Qt::AlignVCenter),
                    tfm.elidedText(type, Qt::ElideRight, typeRect.width()));
    }
    p->restore();
    paintSortMark(p, rect, logicalIndex);
}

void TypedHeaderView::paintSortMark(QPainter* p, const QRect& rect, int logicalIndex) const {
    if (!model() || !rect.isValid()) return;
    const QString mark = model()->headerData(logicalIndex, orientation(), SortMarkRole).toString();
    if (mark.isEmpty()) return;
    QFont f = typeFont();
    f.setItalic(false);
    f.setBold(true);
    p->save();
    p->setFont(f);
    p->setPen(palette().color(QPalette::Highlight));
    p->drawText(rect.adjusted(2, 1, -4, -1), Qt::AlignRight | Qt::AlignTop, mark);
    p->restore();
}

QSize TypedHeaderView::sectionSizeFromContents(int logicalIndex) const {
    QSize s = QHeaderView::sectionSizeFromContents(logicalIndex);
    const QString type = typeOf(logicalIndex);
    if (type.isEmpty()) return s;
    const QFontMetrics tfm(typeFont());
    // The base size holds the name and the margins around it; the type line
    // adds its height, and its width where it is the wider line.
    const int nameW = fontMetrics().horizontalAdvance(
        model()->headerData(logicalIndex, orientation(), Qt::DisplayRole).toString());
    s.setWidth(std::max(s.width(), s.width() - nameW + tfm.horizontalAdvance(type)));
    s.setHeight(s.height() + tfm.height());
    return s;
}
