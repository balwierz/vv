#include "typedheader.h"

#include <QPainter>
#include <QStyleOptionHeader>

TypedHeaderView::TypedHeaderView(QWidget* parent) : QHeaderView(Qt::Horizontal, parent) {}

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
    const Qt::Alignment h = opt.textAlignment & Qt::AlignHorizontal_Mask;
    p->drawText(typeRect, int(h | Qt::AlignVCenter),
                tfm.elidedText(type, Qt::ElideRight, typeRect.width()));
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
