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
#include <QFutureWatcher>
#include <QRegularExpression>
#include <atomic>
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
    ~ArrowTableModel() override;

    // Cell value without display formatting (no digit grouping), for the
    // clipboard — pasting "1000000" is more useful than "1_000_000".
    enum { RawTextRole = Qt::UserRole + 1 };

    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    // Sort by a display column (typed). displayCol < 0 clears the sort.
    // *Sync* variant: computes inline (used by the headless self-test).
    void    sortByDisplayColumn(int displayCol, Qt::SortOrder order);
    int     sortColumn() const { return sortCol_; }

    // Live filter using vv's --filter DSL (already parsed). clearFilter()
    // restores the unfiltered view. *Sync* variants compute inline.
    void    setFilter(const FilterExpr& expr);
    void    clearFilter();
    bool    hasFilter() const { return hasFilter_; }

    // Async variants: filter_rows / readFullColumn / sort_indices run on a
    // worker thread while the model is "blanked" (rowCount()==0, data()=={})
    // so the UI thread never touches the source concurrently; the result is
    // installed on completion. Emit recomputeStarted/Finished/Canceled for a
    // progress indicator. The GUI uses these; cancelRecompute() aborts.
    void    setFilterAsync(const FilterExpr& expr);
    void    clearFilterAsync();
    void    sortAsyncByDisplayColumn(int displayCol, Qt::SortOrder order);
    void    cancelRecompute();
    bool    isComputing() const { return computing_; }

    // Async find: scans the whole view off the UI thread (same blank-while-
    // computing discipline as the filter/sort path) and builds a sorted list of
    // match positions. String columns are vectorized with Arrow
    // match_substring as a *candidate* prefilter for literal needles; the Qt
    // regex on the displayed cell text is always the source of truth. Emits
    // findReady(matches) on completion; cancelRecompute() aborts. Once a scan
    // for a pattern is done, findNext() navigates it in O(log matches) without
    // touching the source. The sync findNext (self-test) triggers a synchronous
    // scan first if the results are stale (see findResultsValid).
    void    findAsync(const QRegularExpression& re);
    bool    findResultsValid(const QRegularExpression& re) const;
    qint64  findMatchCount() const { return (qint64)findPos_.size(); }

signals:
    void    recomputeStarted();
    void    recomputeFinished(qint64 viewRows);
    void    recomputeCanceled();
    void    findReady(qint64 matches);

public:

    // Search overlay: matching cells are highlighted (Qt regex over the visible
    // cells, the source of truth). findNext walks the precomputed match list
    // (findAsync) in O(log matches); if no scan has run for the active pattern
    // it computes one synchronously (the self-test path).
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
    // Map a source row to its chunk index in O(log chunks) via a cumulative
    // first-row offset table (chunkFirstRow_), built lazily and extended
    // forward for streaming sources. Returns -1 if the row is out of range.
    int                chunkIndexForRow(int64_t srcRow) const;
    int64_t   sourceRow(int viewRow) const {
        return order_.empty() ? (int64_t)viewRow : order_[viewRow];
    }
    QString   cellText(int viewRow, int dispCol) const;
    QString   rawCellText(int viewRow, int dispCol) const;
    void      drainStreaming() const;
    std::shared_ptr<arrow::ChunkedArray> readFullColumn(int srcCol) const;
    void      reseedRowCount();
    void      rebuildOrder();             // sync: order_ = computeOrderVec(...)
    // Pure computation of the display→source permutation from a filter + sort.
    // Runs on a worker thread for the async path; polls `cancel` between phases
    // and returns {} (identity) if aborted. Drives the source exclusively while
    // the model is blanked, so no UI-thread source access races it.
    std::vector<int64_t> computeOrderVec(FilterExpr filter, bool hasFilter,
                                         int sortCol, Qt::SortOrder order,
                                         std::atomic<bool>* cancel) const;
    void      startRecompute();           // blank + launch worker
    void      onRecomputeDone();          // install the worker's result
    // Pure find computation: scan the view (chunk by chunk in source order),
    // returning the matching cell positions (viewRow*cols + col), sorted. Polls
    // `cancel`; returns {} if aborted. Runs on the worker for findAsync and
    // inline for the synchronous findNext fallback.
    std::vector<int64_t> computeFindPos(QRegularExpression re,
                                        std::atomic<bool>* cancel) const;
    void      invalidateFind();           // drop stale match list (view changed)

    std::unique_ptr<TabularSource> src_;
    std::shared_ptr<arrow::Schema> schema_;
    std::vector<int>               displayCols_;
    std::vector<QString>           colNames_;
    std::vector<QString>           colTypes_;

    mutable std::map<int, LoadedChunk> cache_;
    mutable std::list<int>             lru_;
    static constexpr int               kMaxCache = 8;
    // chunkFirstRow_[c] == chunk_meta(c).first_row, ascending. Indexes source
    // rows → chunks; invariant under sort/filter (those only permute order_),
    // so it is cleared only when the source rebuilds (stepSlice / re-open).
    mutable std::vector<int64_t>       chunkFirstRow_;

    std::vector<int64_t> order_;          // display row -> source row; empty = identity
    int                  sortCol_   = -1;
    Qt::SortOrder        sortOrder_ = Qt::AscendingOrder;
    FilterExpr           filter_;
    bool                 hasFilter_ = false;

    QRegularExpression   searchRe_;
    bool                 hasSearch_ = false;

    // Async recompute (filter/sort/find off the UI thread). One worker at a
    // time; pendingJob_ tells onRecomputeDone how to install the result.
    enum class Job { None, Order, Find };
    QFutureWatcher<std::vector<int64_t>>* watcher_ = nullptr;
    std::shared_ptr<std::atomic<bool>>    cancel_;   // worker holds a copy
    bool                                  computing_ = false;
    Job                                   pendingJob_ = Job::None;
    QRegularExpression                    pendingRe_;     // for Job::Find

    // Precomputed match positions (viewRow*cols + col), ascending. Valid for
    // findPattern_ until the view (order_) or source changes.
    std::vector<int64_t>                  findPos_;
    QString                               findPattern_;
    bool                                  findValid_ = false;

    mutable int64_t loaded_rows_  = 0;
    mutable bool    fully_loaded_ = false;
};
