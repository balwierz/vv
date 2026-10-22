// ArrowTableModel — a QAbstractTableModel backed by a vv TabularSource.
//
// Wraps any libvvcore source (Parquet, Arrow, CSV, BAM, HDF5, NPZ, …) for
// display in a QTableView. Rows/columns map onto the source; cell text is
// decoded lazily, chunk by chunk, with a small LRU cache mirroring the TUI.
//
// View state is a single display→source permutation (order_) combining the
// live filter (vvcore::filter_rows) and the typed sort (vvcore::stable_sort_order,
// hand-rolled — arrow::compute's kernels aren't registered in this linkage) —
// filter first, then sort within the kept rows, just like the TUI's
// rebuild_display_order. Search is overlaid as a cell highlight
// (Qt::BackgroundRole) plus a findNext() cursor.
#pragma once

#include <QAbstractTableModel>
#include <QRegularExpression>
#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "sourceowner.h"
#include "vv/vvcore.hpp"

class ArrowTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    // `owner` runs every call into `src`; the tabs of one file share one
    // (their sources share handles). Without one the model makes its own.
    // `async`: the model reads its schema on the owner thread (until then it
    // has no rows or columns and isReady() is false) and loads the chunks the
    // view asks for there, showing "…" until they arrive. Otherwise every
    // read waits for the owner (the self-test's synchronous model).
    explicit ArrowTableModel(std::unique_ptr<TabularSource> src,
                             QObject* parent = nullptr,
                             std::shared_ptr<SourceOwner> owner = {},
                             bool async = false);
    ~ArrowTableModel() override;

    // Cell value without display formatting (no digit grouping), for the
    // clipboard — pasting "1000000" is more useful than "1_000_000".
    // RawTextRole: the cell without digit grouping. NumericRole: the cell as
    // a double for a column summableDigits() accepts; invalid otherwise.
    enum { RawTextRole = Qt::UserRole + 1, NumericRole = Qt::UserRole + 2 };
    // Significant digits a sum over this display column is shown with: 19
    // for integers (exact), 7 for float32 / float16, 15 for other numbers;
    // 0 when its cells are not summed (text, dates, times, nested types).
    int summableDigits(int displayCol) const;

    int      rowCount(const QModelIndex& parent = {}) const override;
    // Qt's views count rows, and the header's pixel length, in int: a view
    // whose rows are h pixels high can show INT_MAX / h rows (93 M at 23 px);
    // beyond that QHeaderView's geometry overflows and it repaints without
    // end. A larger table shows its first maxViewRows() rows, and footer()
    // says so. Set before a view is attached.
    static constexpr int64_t kMaxViewRows = 2'000'000'000LL;
    void     setMaxViewRows(int64_t n);
    int64_t  maxViewRows() const { return maxViewRows_; }
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    // Horizontal header: DisplayRole the column name, TypeRole its type
    // (the second line TypedHeaderView draws), ToolTipRole both.
    static constexpr int TypeRole = Qt::UserRole + 1;
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

    // Async variants: filter_rows / sort_rows_by_column run on the source's
    // owner thread while the model is "blanked" (rowCount()==0, data()=={});
    // the result is installed on completion. A job started while another runs
    // cancels it and queues behind it, so one source never has two readers.
    // Emit recomputeStarted/Finished/Canceled for a progress indicator. The
    // GUI uses these; cancelRecompute() aborts.
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

    // The schema has arrived (always true for a synchronous model).
    bool    isReady() const { return !loading_; }
    // Chunks requested for the view and not yet delivered.
    bool    hasPendingReads() const { return !wantRows_.empty() || !inflight_.empty(); }
    // Batches of rows sent to the owner thread so far (the self-test's
    // measure of a view that keeps re-reading).
    int64_t readBatches() const { return readBatches_; }
    // Rows 0..n-1 of the source, all in the chunk cache (identity view only):
    // what column autosizing may sample without asking for more.
    int64_t loadedPrefixRows() const;
    // While one exists, a cache miss reads the chunk on the owner and waits
    // for it, as before: for copying a selection, the selection sum, the
    // self-test — callers that need the values now.
    class SyncReads {
    public:
        explicit SyncReads(const ArrowTableModel* m) : m_(m), prev_(m->syncReads_) { m_->syncReads_ = true; }
        ~SyncReads() { m_->syncReads_ = prev_; }
    private:
        const ArrowTableModel* m_;
        bool prev_;
    };

signals:
    void    ready();                       // the schema arrived (async model)
    void    chunksLoaded();                // requested chunks arrived
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

    // Summaries of display columns over the rows the filter keeps, read from
    // this model's own source on the calling thread (summarize_columns, with
    // value counts) — for a tab that cannot be opened again elsewhere. False
    // with *err while a worker owns the source or on a read error.
    bool summarize(const std::vector<int>& displayCols, std::vector<ColumnSummary>* out,
                   QString* err) const;

    // Step the NPZ-style 3-D+ slice axis. Returns true if the source rebuilt.
    bool    stepSlice(int delta);

    QString footer() const;
    QString columnName(int displayCol) const;
    QString columnType(int displayCol) const;
    int     displayColumnCount() const { return (int)displayCols_.size(); }
    int     sourceColumn(int displayCol) const { return displayCols_[(size_t)displayCol]; }
    int64_t sourceTotal() const;          // total source rows (drains streaming)
    int64_t viewRows() const;             // rows after filter

    // The source's metadata, as last read on the owner thread: what the UI
    // reads instead of calling the source. Refreshed after every job.
    const std::shared_ptr<arrow::Schema>& schema() const { return schema_; }
    const std::string&  tabLabel() const     { return meta_.tabLabel; }
    int64_t             totalRows() const    { return meta_.totalRows; }
    const PreviewLimit& previewLimit() const { return meta_.limit; }
    // What this tab offers beyond its file's format options (read with the
    // schema, on the owner thread).
    const TabTraits& tabTraits() const { return meta_.traits; }
    // --select resolved against the source (select_columns), on its owner.
    std::vector<int> selectColumns(const std::string& spec,
                                   std::vector<std::string>* unknown) const;

private:
    // The source and the thread that owns it. Core is shared with the tasks
    // posted to the owner, which keep it alive until they have run; it is
    // only ever touched on the owner thread (or by the UI thread while it
    // waits in runSync).
    struct Core {
        std::unique_ptr<TabularSource> src;
        // chunkFirstRow[c] == chunk_meta(c).first_row, ascending: source rows
        // → chunks, built on demand and extended forward for streams;
        // invariant under sort/filter, cleared when the source rebuilds.
        std::vector<int64_t>           chunkFirstRow;
    };
    std::shared_ptr<SourceOwner> owner_;
    std::shared_ptr<Core>        core_;
    // Results reach the model through the mailbox: the destructor clears
    // `model` under the lock, so a task never posts to a deleted model.
    struct Mailbox { std::mutex m; ArrowTableModel* model = nullptr; };
    std::shared_ptr<Mailbox>     mail_;
    // Run `fn` on the UI thread (queued) if this model still exists.
    template <class F> static void deliver(const std::shared_ptr<Mailbox>& mb, F fn) {
        std::lock_guard<std::mutex> l(mb->m);
        if (mb->model) QMetaObject::invokeMethod(mb->model, std::move(fn), Qt::QueuedConnection);
    }
    template <class F> auto withSource(F fn) const {
        auto core = core_;
        return owner_->runSync([core, &fn] { return fn(*core->src); });
    }
    struct Meta {
        std::string  tabLabel;
        int64_t      totalRows = -1;
        PreviewLimit limit;
        std::string  footer;
        arrow::Status readStatus;
        TabTraits    traits;          // matrix tab / slices / coordinate columns
    };
    Meta meta_;
    static Meta readMeta(TabularSource& s);
    void refreshMeta();

    struct LoadedChunk {
        int                            idx = -1;
        int64_t                        first_row = 0;
        int64_t                        num_rows = 0;
        std::shared_ptr<arrow::Table>  table;     // columns == displayCols_ order
    };

    // The cached chunk holding source row `srcRow`; on a miss, nullptr and the
    // row is requested (or, under SyncReads, read now).
    const LoadedChunk* chunkForRow(int64_t srcRow) const;
    const LoadedChunk* cachedChunk(int64_t srcRow) const;
    const LoadedChunk* insertChunk(LoadedChunk lc) const;
    // On the owner thread: the chunk index of source row `row` (O(log chunks)
    // via core.chunkFirstRow), -1 if out of range; and that chunk read.
    static int  coreChunkIndex(Core& core, int64_t row);
    static bool coreLoadChunk(Core& core, int64_t row, const std::vector<int>& cols,
                              LoadedChunk* out);
    // Asynchronous reads: rows asked for (wantRows_) go to the owner in one
    // task per event-loop turn (flushRequests); a result for an older
    // generation (the cache was cleared since) is dropped.
    void requestRow(int64_t srcRow) const;
    void flushRequests();
    void onChunksLoaded(uint64_t gen, std::vector<LoadedChunk> chunks,
                        std::vector<int64_t> asked, std::vector<int64_t> failed, Meta meta);
    void onInitDone(std::shared_ptr<arrow::Schema> schema, std::vector<std::string> hidden,
                    Meta meta, int64_t loadedRows, bool fullyLoaded);
    void setColumns(const std::vector<std::string>& hidden);
    void onFetched(int64_t newrows, bool fullyLoaded, Meta meta);
    int64_t   sourceRow(int viewRow) const {
        return identity_ ? (int64_t)viewRow : (*order_)[(size_t)viewRow];
    }
    QString   cellText(int viewRow, int dispCol) const;
    QString   rawCellText(int viewRow, int dispCol) const;
    QVariant  cellNumber(int viewRow, int dispCol) const;
    void      drainStreaming() const;
    void      reseedRowCount();
    void      rebuildOrder();             // sync: order_ = computeOrderVec(...)
    // Pure computation of the display→source permutation from a filter + sort.
    // Runs on a worker thread for the async path; polls `cancel` between phases
    // and returns {} (identity) if aborted. Drives the source exclusively while
    // the model is blanked, so no UI-thread source access races it.
    static std::vector<int64_t> computeOrderVec(TabularSource& src, std::vector<int> displayCols,
                                                FilterExpr filter, bool hasFilter,
                                                int sortCol, Qt::SortOrder order,
                                                std::atomic<bool>* cancel);
    void      startRecompute();           // blank + post the job to the owner
    // Install a finished job's result, unless job `id` was superseded.
    void      onRecomputeDone(uint64_t id, std::vector<int64_t> result, Meta meta);
    // Pure find computation: scan the view (chunk by chunk in source order),
    // returning the matching cell positions (viewRow*cols + col), sorted. Polls
    // `cancel`; returns {} if aborted. Runs on the worker for findAsync and
    // inline for the synchronous findNext fallback.
    static std::vector<int64_t> computeFindPos(TabularSource& src, std::vector<int> displayCols,
                                               std::shared_ptr<const std::vector<int64_t>> order,
                                               bool identity, QRegularExpression re,
                                               std::atomic<bool>* cancel);
    void      invalidateFind();           // drop stale match list (view changed)

    std::shared_ptr<arrow::Schema> schema_;
    std::vector<int>               displayCols_;
    std::vector<QString>           colNames_;
    std::vector<QString>           colTypes_;

    mutable std::map<int, LoadedChunk> cache_;
    mutable std::list<int>             lru_;
    // The cache keeps at least kMaxCache chunks, and as many as the view
    // used since the last delivery plus the chunks delivered (up to
    // kMaxCacheCeiling): a sorted or filtered view shows rows of many chunks
    // at once, and evicting one it still shows makes the next paint ask for
    // it again — reads and repaints without end.
    static constexpr int               kMaxCache = 8;
    static constexpr int               kMaxCacheCeiling = 1024;
    mutable int                        cacheCap_ = kMaxCache;
    mutable std::set<int>              hitSinceDelivery_;   // chunk indexes the view read
    mutable int                        lastHit_ = -1;
    // The chunk of the last chunkForRow and the end of its rows; reset with
    // the cache (clearChunkCache), and set to the entry chunkForRow returns,
    // which the eviction there never removes.
    mutable const LoadedChunk*         lastChunk_ = nullptr;
    mutable int64_t                    lastChunkEnd_ = 0;
    void clearChunkCache() const;       // also starts a new read generation

    bool                               async_ = false;
    bool                               loading_ = false;    // async: schema not read yet
    mutable bool                       syncReads_ = false;  // SyncReads in effect
    mutable std::set<int64_t>          wantRows_;           // asked for, not yet sent
    mutable std::set<int64_t>          inflight_;           // sent, not yet delivered
    mutable std::set<int64_t>          failedRows_;         // read failed: no retry
    mutable bool                       flushPending_ = false;
    int64_t                            readBatches_ = 0;
    mutable uint64_t                   gen_ = 0;
    bool                               fetching_ = false;   // async fetchMore in flight
    // The last cell decoded by data(): a view asks for DisplayRole and then
    // BackgroundRole (search highlight) of the same cell. Cleared on every
    // reset / layout / data change.
    mutable int     lastCellRow_ = -1, lastCellCol_ = -1;
    mutable QString lastCellText_;
    const QString& displayText(int viewRow, int dispCol) const;

    // display row -> source row (unless identity_); shared with the jobs that
    // read it (find), which get the vector of the moment they were posted.
    std::shared_ptr<const std::vector<int64_t>> order_ = std::make_shared<std::vector<int64_t>>();
    // The view is the source as-is (no filter, no sort), and order_ is unused.
    // Kept separately from order_.empty(): a filter that matches nothing also
    // leaves order_ empty, and that view has zero rows, not all of them.
    bool                 identity_  = true;
    int                  sortCol_   = -1;
    Qt::SortOrder        sortOrder_ = Qt::AscendingOrder;
    FilterExpr           filter_;
    bool                 hasFilter_ = false;

    QRegularExpression   searchRe_;
    bool                 hasSearch_ = false;

    // Async recompute (filter/sort/find on the owner thread). jobId_ names
    // the job whose result is awaited; pendingJob_ tells onRecomputeDone how
    // to install it. A superseded job's flag is set and its result dropped.
    enum class Job { None, Order, Find };
    uint64_t                              jobId_ = 0;
    std::shared_ptr<std::atomic<bool>>    cancel_;   // the job holds a copy
    bool                                  computing_ = false;
    int64_t                               maxViewRows_ = kMaxViewRows;
    Job                                   pendingJob_ = Job::None;
    bool                                  pendingHasFilter_ = false;  // for Job::Order
    QRegularExpression                    pendingRe_;     // for Job::Find

    // Precomputed match positions (viewRow*cols + col), ascending. Valid for
    // findPattern_ until the view (order_) or source changes.
    std::vector<int64_t>                  findPos_;
    QString                               findPattern_;
    bool                                  findValid_ = false;

    mutable int64_t loaded_rows_  = 0;
    mutable bool    fully_loaded_ = false;
};
