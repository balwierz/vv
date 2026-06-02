// ArrowTableModel — a QAbstractTableModel backed by a vv TabularSource.
//
// Wraps any libvvcore source (Parquet, Arrow, CSV, BAM, HDF5, NPZ, …) for
// display in a QTableView. Rows/columns map onto the source; cell text is
// decoded lazily, chunk by chunk, with a small LRU cache mirroring the TUI.
//
// View state is a single display→source permutation (order_) combining the
// live filter (vvcore::filter_rows) and the typed sort (Arrow
// compute::SortIndices) — filter first, then sort within the kept rows, just
// like the TUI's rebuild_display_order. Search is overlaid as a cell
// highlight (Qt::BackgroundRole) plus a findNext() cursor.
#pragma once

#include <QAbstractTableModel>
#include <QRegularExpression>
#include <list>
#include <map>
#include <memory>
#include <vector>

#include "vv/vvcore.hpp"

class ArrowTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit ArrowTableModel(std::unique_ptr<TabularSource> src,
                             QObject* parent = nullptr);

    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    // Sort by a display column (typed). displayCol < 0 clears the sort.
    void    sortByDisplayColumn(int displayCol, Qt::SortOrder order);
    int     sortColumn() const { return sortCol_; }

    // Live filter using vv's --filter DSL (already parsed). clearFilter()
    // restores the unfiltered view.
    void    setFilter(const FilterExpr& expr);
    void    clearFilter();
    bool    hasFilter() const { return hasFilter_; }

    // Search overlay: matching cells are highlighted; findNext walks matches.
    void        setSearch(const QRegularExpression& re);
    void        clearSearch();
    QModelIndex findNext(const QModelIndex& from, bool forward) const;

    // Per-column statistics (count/nulls/min/max/mean/distinct) for the
    // stats panel. displayCol maps to the underlying source field.
    ColStats columnStats(int displayCol) const;

    // Step the NPZ-style 3-D+ slice axis. Returns true if the source rebuilt.
    bool    stepSlice(int delta);

    QString footer() const;
    QString columnName(int displayCol) const;
    QString columnType(int displayCol) const;
    int     displayColumnCount() const { return (int)displayCols_.size(); }
    int64_t sourceTotal() const;          // total source rows (drains streaming)
    int64_t viewRows() const;             // rows after filter
    TabularSource* source() const { return src_.get(); }

private:
    struct LoadedChunk {
        int                            idx = -1;
        int64_t                        first_row = 0;
        std::shared_ptr<arrow::Table>  table;     // columns == displayCols_ order
    };

    const LoadedChunk* chunkForRow(int64_t srcRow) const;
    int64_t   sourceRow(int viewRow) const {
        return order_.empty() ? (int64_t)viewRow : order_[viewRow];
    }
    QString   cellText(int viewRow, int dispCol) const;
    void      drainStreaming() const;
    std::shared_ptr<arrow::ChunkedArray> readFullColumn(int srcCol) const;
    void      reseedRowCount();
    void      rebuildOrder();             // recompute order_ from filter + sort

    std::unique_ptr<TabularSource> src_;
    std::shared_ptr<arrow::Schema> schema_;
    std::vector<int>               displayCols_;
    std::vector<QString>           colNames_;
    std::vector<QString>           colTypes_;

    mutable std::map<int, LoadedChunk> cache_;
    mutable std::list<int>             lru_;
    static constexpr int               kMaxCache = 8;

    std::vector<int64_t> order_;          // display row -> source row; empty = identity
    int                  sortCol_   = -1;
    Qt::SortOrder        sortOrder_ = Qt::AscendingOrder;
    FilterExpr           filter_;
    bool                 hasFilter_ = false;

    QRegularExpression   searchRe_;
    bool                 hasSearch_ = false;

    mutable int64_t loaded_rows_  = 0;
    mutable bool    fully_loaded_ = false;
};
