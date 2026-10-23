// TypedHeaderView — a table's column header with each column's type on a
// second line, smaller, italic and in the placeholder text colour, so it does
// not read as part of the name. The type comes from the model's TypeRole;
// setShowTypes(false) gives one-line headers. A column that is one key of a
// sort by several columns shows its rank and direction ("▲1", "▼2") in its
// top-right corner (the model's SortMarkRole).
#pragma once

#include <QHash>
#include <QHeaderView>
#include <QList>
#include <QPair>

class TypedHeaderView : public QHeaderView {
public:
    static constexpr int TypeRole = Qt::UserRole + 1;       // = ArrowTableModel::TypeRole
    static constexpr int SortMarkRole = Qt::UserRole + 3;   // = ArrowTableModel::SortMarkRole

    explicit TypedHeaderView(QWidget* parent = nullptr);
    void setShowTypes(bool on);
    bool showTypes() const { return show_; }
    // The type line's font: the header's, smaller and italic.
    QFont typeFont() const;
    // Column `logicalIndex` holds one character per `advance` pixels from
    // `inset` on (an alignment's sequence): its type line is a ruler of
    // positions (1, 10, 20, …) instead. advance 0 removes it.
    void setRuler(int logicalIndex, int advance, int inset);
    // The positions the ruler draws for a section `width` pixels wide, and
    // where (x from the section's left edge).
    QList<QPair<int, int>> rulerMarks(int logicalIndex, int width) const;

protected:
    void  paintSection(QPainter* p, const QRect& rect, int logicalIndex) const override;
    QSize sectionSizeFromContents(int logicalIndex) const override;

private:
    QString typeOf(int logicalIndex) const;
    QHash<int, QPair<int, int>> rulers_;   // logical index → (advance, inset)
    void    paintSortMark(QPainter* p, const QRect& rect, int logicalIndex) const;
    bool show_ = true;
};
