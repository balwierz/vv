// TableView — a table view whose first columns can be frozen: they stay at
// the left edge while the rest scrolls horizontally. The frozen columns are a
// second view laid over the left of this one (Qt's frozen-column recipe):
// the same model, selection model and delegate, its own TypedHeaderView, no
// scroll bars, scrolled vertically with this view and given the frozen
// columns' widths, order and the type line.
#pragma once

#include <QList>
#include <QTableView>

class QTimer;

class TableView : public QTableView {
    Q_OBJECT
public:
    explicit TableView(QWidget* parent = nullptr);
    void setModel(QAbstractItemModel* model) override;
    // Freeze the first `n` visible columns in on-screen order; 0 unfreezes.
    void setFrozenColumns(int n);
    int  frozenColumns() const { return frozen_; }
    // The view drawn over the frozen columns (hidden when none are), for
    // its header's clicks and menus.
    QTableView* frozenView() const { return overlay_; }
    QList<int>  frozenLogical() const { return cols_; }   // the frozen columns

    void scrollTo(const QModelIndex& index, ScrollHint hint = EnsureVisible) override;

protected:
    void resizeEvent(QResizeEvent* e) override;
    void updateGeometries() override;

private:
    void sync();            // the overlay's columns, order, widths, geometry
    void place();           // its geometry and vertical scroll
    int  frozenWidth() const;

    QTableView* overlay_;
    QTimer*     syncTimer_;  // coalesces column hides / moves
    int         frozen_ = 0;
    QList<int>  cols_;       // the frozen logical columns, in order
};
