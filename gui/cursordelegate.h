// CursorColumnDelegate — the table views' delegate.
#pragma once

#include <QPainter>
#include <QStyledItemDelegate>
#include <QTableView>

// Tints the cursor column, the one the Column tab summarises, so it stands
// out from its neighbours; selection and search highlights paint over it.
class CursorColumnDelegate : public QStyledItemDelegate {
public:
    explicit CursorColumnDelegate(QTableView* view) : QStyledItemDelegate(view), view_(view) {}
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        if (idx.column() == view_->currentIndex().column()) {
            QColor c = opt.palette.color(QPalette::Highlight);
            c.setAlpha(40);
            p->fillRect(opt.rect, c);
        }
        QStyledItemDelegate::paint(p, opt, idx);
    }
private:
    QTableView* view_;
};
