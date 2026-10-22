// TypedHeaderView — a table's column header with each column's type on a
// second line, smaller, italic and in the placeholder text colour, so it does
// not read as part of the name. The type comes from the model's TypeRole;
// setShowTypes(false) gives one-line headers.
#pragma once

#include <QHeaderView>

class TypedHeaderView : public QHeaderView {
public:
    static constexpr int TypeRole = Qt::UserRole + 1;   // = ArrowTableModel::TypeRole

    explicit TypedHeaderView(QWidget* parent = nullptr);
    void setShowTypes(bool on);
    bool showTypes() const { return show_; }
    // The type line's font: the header's, smaller and italic.
    QFont typeFont() const;

protected:
    void  paintSection(QPainter* p, const QRect& rect, int logicalIndex) const override;
    QSize sectionSizeFromContents(int logicalIndex) const override;

private:
    QString typeOf(int logicalIndex) const;
    bool show_ = true;
};
