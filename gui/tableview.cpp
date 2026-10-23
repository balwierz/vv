#include "tableview.h"

#include "typedheader.h"

#include <QHeaderView>
#include <QScrollBar>
#include <QTimer>

TableView::TableView(QWidget* parent)
    : QTableView(parent), overlay_(new QTableView(this)), syncTimer_(new QTimer(this)) {
    overlay_->setHorizontalHeader(new TypedHeaderView(overlay_));
    overlay_->verticalHeader()->hide();
    overlay_->setFocusPolicy(Qt::NoFocus);
    overlay_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    overlay_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    overlay_->setFrameShape(QFrame::NoFrame);
    overlay_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    overlay_->hide();
    viewport()->stackUnder(overlay_);
    syncTimer_->setSingleShot(true);
    syncTimer_->setInterval(0);
    connect(syncTimer_, &QTimer::timeout, this, [this] { sync(); });

    // Scrolled together, whichever is scrolled (the wheel over the overlay),
    // while the overlay shows. A hidden overlay keeps a range of its own (in
    // rows, and stale): passing values to it then clamped this view's
    // per-pixel position to the row count and moved it to an unrelated row.
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        if (overlay_->isVisible()) overlay_->verticalScrollBar()->setValue(v);
    });
    connect(overlay_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        if (overlay_->isVisible()) verticalScrollBar()->setValue(v);
    });
    QHeaderView* h = horizontalHeader();
    connect(h, &QHeaderView::sectionResized, this, [this](int logical, int oldSize, int newSize) {
        if (!frozen_) return;
        if (oldSize == 0 || newSize == 0) { syncTimer_->start(); return; }   // hidden / shown
        if (cols_.contains(logical)) {
            overlay_->setColumnWidth(logical, newSize);
            place();
        }
    });
    connect(h, &QHeaderView::sectionMoved, this, [this] { if (frozen_) syncTimer_->start(); });
}

void TableView::setModel(QAbstractItemModel* model) {
    QTableView::setModel(model);
    overlay_->setModel(model);
    overlay_->setSelectionModel(selectionModel());
    if (model) {
        connect(model, &QAbstractItemModel::modelReset, this, [this] { if (frozen_) syncTimer_->start(); });
        connect(model, &QAbstractItemModel::columnsInserted, this, [this] { if (frozen_) syncTimer_->start(); });
    }
}

void TableView::setFrozenColumns(int n) {
    frozen_ = std::max(0, n);
    sync();
}

int TableView::frozenWidth() const {
    int w = 0;
    for (int c : cols_) w += columnWidth(c);
    return w;
}

void TableView::sync() {
    QHeaderView* mh = horizontalHeader();
    QHeaderView* oh = overlay_->horizontalHeader();
    cols_.clear();
    for (int vis = 0; frozen_ && vis < mh->count() && cols_.size() < frozen_; ++vis) {
        const int c = mh->logicalIndex(vis);
        if (!isColumnHidden(c)) cols_ << c;
    }
    if (cols_.isEmpty() || !model()) {
        overlay_->hide();
        return;
    }
    // What this view draws its cells with, read again: the row height and
    // scroll mode can change after the overlay was made.
    overlay_->setItemDelegate(itemDelegate());
    overlay_->setAlternatingRowColors(alternatingRowColors());
    overlay_->setSelectionBehavior(selectionBehavior());
    overlay_->setSelectionMode(selectionMode());
    overlay_->setVerticalScrollMode(verticalScrollMode());
    overlay_->verticalHeader()->setMinimumSectionSize(verticalHeader()->minimumSectionSize());
    overlay_->verticalHeader()->setDefaultSectionSize(verticalHeader()->defaultSectionSize());
    for (int c = 0; c < oh->count(); ++c) overlay_->setColumnHidden(c, !cols_.contains(c));
    for (int i = 0; i < cols_.size(); ++i) {
        oh->moveSection(oh->visualIndex(cols_[i]), i);
        overlay_->setColumnWidth(cols_[i], columnWidth(cols_[i]));
    }
    overlay_->show();
    place();
}

void TableView::place() {
    if (!overlay_->isVisible() && cols_.isEmpty()) return;
    auto* mt = dynamic_cast<TypedHeaderView*>(horizontalHeader());
    auto* ot = dynamic_cast<TypedHeaderView*>(overlay_->horizontalHeader());
    if (mt && ot) ot->setShowTypes(mt->showTypes());
    overlay_->horizontalHeader()->setFixedHeight(horizontalHeader()->height());
    overlay_->setGeometry(verticalHeader()->width() + frameWidth(), frameWidth(),
                          std::min(frozenWidth(), viewport()->width()),
                          viewport()->height() + horizontalHeader()->height());
    overlay_->raise();   // over this view's header, which a caller may set after the overlay
    overlay_->verticalScrollBar()->setValue(verticalScrollBar()->value());
}

void TableView::resizeEvent(QResizeEvent* e) {
    QTableView::resizeEvent(e);
    if (frozen_) place();
}

void TableView::updateGeometries() {
    QTableView::updateGeometries();
    if (frozen_) place();
}

// A frozen cell needs no horizontal scroll; any other one is kept out from
// under the frozen columns.
void TableView::scrollTo(const QModelIndex& index, ScrollHint hint) {
    if (!frozen_ || !index.isValid() || cols_.isEmpty()) {
        QTableView::scrollTo(index, hint);
        return;
    }
    QScrollBar* hs = horizontalScrollBar();
    const int keep = hs->value();
    QTableView::scrollTo(index, hint);
    if (cols_.contains(index.column())) {
        hs->setValue(keep);
        return;
    }
    const int left = columnViewportPosition(index.column());
    if (left < frozenWidth()) {
        const int per = horizontalScrollMode() == ScrollPerPixel ? frozenWidth() - left : 0;
        if (per > 0) hs->setValue(hs->value() - per);
        else {
            // Per-item: step back a column at a time until it is clear.
            while (hs->value() > 0 && columnViewportPosition(index.column()) < frozenWidth())
                hs->setValue(hs->value() - 1);
        }
    }
}
