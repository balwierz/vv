#include "arrowtablemodel.h"

#include <QBrush>
#include <QColor>
#include <QLocale>
#include <QString>
#include <algorithm>

namespace {
std::string chunked_cell(const arrow::ChunkedArray& ca, int64_t i) {
    for (const auto& a : ca.chunks()) {
        if (i < a->length()) return cell_to_display_string(*a, i);
        i -= a->length();
    }
    return {};
}

std::string chunked_cell_raw(const arrow::ChunkedArray& ca, int64_t i) {
    for (const auto& a : ca.chunks()) {
        if (i < a->length()) return cell_to_string(*a, i);   // no digit grouping
        i -= a->length();
    }
    return {};
}

}  // namespace

ArrowTableModel::ArrowTableModel(std::unique_ptr<TabularSource> src,
                                 QObject* parent, std::shared_ptr<SourceOwner> owner)
    : QAbstractTableModel(parent),
      owner_(owner ? std::move(owner) : std::make_shared<SourceOwner>()),
      core_(std::make_shared<Core>()),
      mail_(std::make_shared<Mailbox>()) {
    core_->src = std::move(src);
    mail_->model = this;
    std::vector<std::string> hide;
    withSource([&](TabularSource& s) {
        schema_ = s.schema();
        hide = s.hidden_for_display();
        meta_ = readMeta(s);
        return 0;
    });
    for (int i = 0; i < schema_->num_fields(); ++i) {
        const auto& f = schema_->field(i);
        bool is_hidden = false;
        for (const auto& h : hide) if (h == f->name()) { is_hidden = true; break; }
        if (is_hidden) continue;
        displayCols_.push_back(i);
        colNames_.push_back(QString::fromStdString(f->name()));
        colTypes_.push_back(QString::fromStdString(type_label(*f->type())));
    }
    reseedRowCount();
    auto forget = [this] { lastCellRow_ = lastCellCol_ = -1; lastCellText_.clear(); };
    connect(this, &QAbstractItemModel::modelAboutToBeReset, this, forget);
    connect(this, &QAbstractItemModel::layoutAboutToBeChanged, this, forget);
    connect(this, &QAbstractItemModel::dataChanged, this, forget);
}

ArrowTableModel::~ArrowTableModel() {
    // A job still queued or running checks its flag and stops at its next
    // chunk; its result is not delivered (the mailbox is closed). The source
    // is destroyed on its owner thread, after the tasks posted before. If
    // this was the file's last tab, dropping owner_ joins the thread.
    if (cancel_) cancel_->store(true);
    {
        std::lock_guard<std::mutex> l(mail_->m);
        mail_->model = nullptr;
    }
    owner_->post([core = std::move(core_)]() mutable { core.reset(); });
}

ArrowTableModel::Meta ArrowTableModel::readMeta(TabularSource& s) {
    Meta m;
    m.tabLabel   = s.tab_label();
    m.totalRows  = s.total_rows();
    m.limit      = s.preview_limit();
    m.footer     = s.footer();
    m.readStatus = s.read_status();
    return m;
}

void ArrowTableModel::refreshMeta() {
    meta_ = withSource([](TabularSource& s) { return readMeta(s); });
}

void ArrowTableModel::reseedRowCount() {
    withSource([this](TabularSource& s) {
        loaded_rows_ = 0;
        fully_loaded_ = false;
        int64_t tr = s.total_rows();
        if (tr >= 0) { loaded_rows_ = tr; fully_loaded_ = true; return 0; }
        s.ensure(0);
        for (int c = 0; c < s.num_chunks(); ++c) {
            ChunkMeta m = s.chunk_meta(c);
            loaded_rows_ = std::max(loaded_rows_, m.first_row + m.num_rows);
        }
        if (s.total_rows() >= 0) { loaded_rows_ = s.total_rows(); fully_loaded_ = true; }
        return 0;
    });
    refreshMeta();
}

static void drain(TabularSource& s) {
    if (s.total_rows() >= 0) return;
    while (true) {
        int n = s.num_chunks();
        s.ensure(n);
        if (s.num_chunks() == n) break;
    }
}

void ArrowTableModel::drainStreaming() const {
    withSource([](TabularSource& s) { drain(s); return 0; });
}

int64_t ArrowTableModel::sourceTotal() const {
    if (computing_) return loaded_rows_;   // a job owns the source
    const int64_t n = withSource([](TabularSource& s) {
        drain(s);
        int64_t tr = s.total_rows();
        if (tr >= 0) return tr;
        int64_t rows = 0;
        for (int c = 0; c < s.num_chunks(); ++c) {
            ChunkMeta m = s.chunk_meta(c);
            rows = std::max(rows, m.first_row + m.num_rows);
        }
        return rows;
    });
    const_cast<ArrowTableModel*>(this)->refreshMeta();
    return n;
}

int64_t ArrowTableModel::viewRows() const {
    if (!identity_) return (int64_t)order_->size();
    return meta_.totalRows >= 0 ? meta_.totalRows : loaded_rows_;
}

// Called on the owner thread (inside chunkForRow's runSync, with the UI
// thread waiting), so it may use the source and the model's caches.
int ArrowTableModel::chunkIndexForRow(int64_t row) const {
    if (row < 0) return -1;
    TabularSource* src_ = core_->src.get();
    // Extend the offset table to cover every currently-known chunk.
    auto syncIndex = [&] {
        int n = src_->num_chunks();
        for (int c = (int)chunkFirstRow_.size(); c < n; ++c)
            chunkFirstRow_.push_back(src_->chunk_meta(c).first_row);
    };
    src_->ensure(0);
    syncIndex();
    // For streaming sources, pull more chunks forward until `row` is within the
    // known frontier (or the stream is exhausted) — mirrors the old forward scan.
    while (!chunkFirstRow_.empty()) {
        int last = (int)chunkFirstRow_.size() - 1;
        ChunkMeta lm = src_->chunk_meta(last);
        if (row < lm.first_row + lm.num_rows) break;          // covered
        int before = src_->num_chunks();
        src_->ensure(before);
        if (src_->num_chunks() == before) break;              // exhausted
        syncIndex();
    }
    if (chunkFirstRow_.empty()) return -1;
    // Last chunk whose first_row <= row (O(log chunks)).
    auto it = std::upper_bound(chunkFirstRow_.begin(), chunkFirstRow_.end(), row);
    if (it == chunkFirstRow_.begin()) return -1;
    int c = (int)(std::prev(it) - chunkFirstRow_.begin());
    ChunkMeta m = src_->chunk_meta(c);
    if (row < m.first_row || row >= m.first_row + m.num_rows) return -1;
    return c;
}

const ArrowTableModel::LoadedChunk* ArrowTableModel::chunkForRow(int64_t row) const {
    // A view paints cell after cell of the same rows: the chunk of the last
    // lookup answers without asking the source (whose calls take the HDF5
    // lock on an HDF5 tab) or touching the LRU list.
    if (lastChunk_ && row >= lastChunk_->first_row && row < lastChunkEnd_) return lastChunk_;
    int c = -1;
    ChunkMeta m;
    std::shared_ptr<arrow::Table> tbl;
    const bool cached = withSource([&](TabularSource& s) {
        c = chunkIndexForRow(row);
        if (c < 0) return false;
        m = s.chunk_meta(c);
        if (cache_.count(c)) return true;
        if (!s.read_chunk(c, displayCols_, &tbl).ok()) tbl.reset();
        return false;
    });
    if (c < 0) return nullptr;
    auto it = cache_.find(c);
    if (!cached) {
        if (!tbl) return nullptr;
        it = cache_.emplace(c, LoadedChunk{c, m.first_row, std::move(tbl)}).first;
        lru_.push_front(c);
        while ((int)lru_.size() > kMaxCache) {
            int victim = lru_.back();
            lru_.pop_back();
            if (victim != c) cache_.erase(victim);
        }
    } else {
        lru_.remove(c);
        lru_.push_front(c);
    }
    lastChunk_ = &it->second;   // std::map nodes stay put until erased
    lastChunkEnd_ = m.first_row + m.num_rows;
    return lastChunk_;
}

void ArrowTableModel::clearChunkCache() const {
    cache_.clear();
    lru_.clear();
    lastChunk_ = nullptr;
}

QString ArrowTableModel::cellText(int viewRow, int dispCol) const {
    const LoadedChunk* lc = chunkForRow(sourceRow(viewRow));
    if (!lc || !lc->table) return {};
    if (dispCol < 0 || dispCol >= lc->table->num_columns()) return {};
    int64_t local = sourceRow(viewRow) - lc->first_row;
    std::string raw = chunked_cell(*lc->table->column(dispCol), local);
    // format_cell reads only what the source fixed at open (column roles,
    // digit grouping), so the UI thread calls it directly.
    raw = core_->src->format_cell(displayCols_[dispCol], std::move(raw));
    return QString::fromStdString(raw);
}

QVariant ArrowTableModel::cellNumber(int viewRow, int dispCol) const {
    if (summableDigits(dispCol) == 0) return {};
    const LoadedChunk* lc = chunkForRow(sourceRow(viewRow));
    if (!lc || !lc->table) return {};
    if (dispCol < 0 || dispCol >= lc->table->num_columns()) return {};
    int64_t local = sourceRow(viewRow) - lc->first_row;
    for (const auto& a : lc->table->column(dispCol)->chunks()) {
        if (local < a->length()) {
            double d;
            if (!array_value_as_double(*a, local, &d)) return {};
            return d;
        }
        local -= a->length();
    }
    return {};
}

int ArrowTableModel::summableDigits(int displayCol) const {
    if (displayCol < 0 || displayCol >= (int)displayCols_.size()) return 0;
    const arrow::DataType* t = schema_->field(displayCols_[displayCol])->type().get();
    if (t->id() == arrow::Type::DICTIONARY)
        t = static_cast<const arrow::DictionaryType&>(*t).value_type().get();
    const auto id = t->id();
    if (!is_numeric_type(id) || is_date_or_timestamp(*t) ||
        id == arrow::Type::TIME32 || id == arrow::Type::TIME64)
        return 0;
    if (arrow::is_integer(id)) return 19;
    if (id == arrow::Type::FLOAT || id == arrow::Type::HALF_FLOAT) return 7;
    return 15;
}

QString ArrowTableModel::rawCellText(int viewRow, int dispCol) const {
    const LoadedChunk* lc = chunkForRow(sourceRow(viewRow));
    if (!lc || !lc->table) return {};
    if (dispCol < 0 || dispCol >= lc->table->num_columns()) return {};
    int64_t local = sourceRow(viewRow) - lc->first_row;
    std::string raw = chunked_cell_raw(*lc->table->column(dispCol), local);
    raw = core_->src->format_cell(displayCols_[dispCol], std::move(raw));
    return QString::fromStdString(raw);
}

const QString& ArrowTableModel::displayText(int viewRow, int dispCol) const {
    if (viewRow != lastCellRow_ || dispCol != lastCellCol_) {
        lastCellText_ = cellText(viewRow, dispCol);
        lastCellRow_ = viewRow; lastCellCol_ = dispCol;
    }
    return lastCellText_;
}

void ArrowTableModel::setMaxViewRows(int64_t n) {
    maxViewRows_ = std::clamp<int64_t>(n, 1, kMaxViewRows);
}

int ArrowTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    if (computing_) return 0;   // blanked while a worker recomputes order_
    int64_t n = viewRows();
    return (int)std::min(n, maxViewRows_);
}

int ArrowTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return (int)displayCols_.size();
}

QVariant ArrowTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || computing_) return {};
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
        return displayText(index.row(), index.column());
    if (role == RawTextRole)
        return rawCellText(index.row(), index.column());
    if (role == NumericRole)
        return cellNumber(index.row(), index.column());
    if (role == Qt::BackgroundRole && hasSearch_) {
        if (searchRe_.match(displayText(index.row(), index.column())).hasMatch())
            return QBrush(QColor(255, 235, 130));   // soft yellow highlight
    }
    return {};
}

QVariant ArrowTableModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o == Qt::Horizontal) {
        if (section < 0 || section >= (int)colNames_.size()) return {};
        if (role == Qt::DisplayRole) {
            // A nested type (struct<GT: string, AD: list<…>, …>) would size
            // the column to its full text: elide it; the tooltip has it all.
            const QString& t = colTypes_[section];
            return colNames_[section] + "\n"
                 + (t.size() > 32 ? t.left(31) + QChar(0x2026) : t);
        }
        if (role == Qt::ToolTipRole)
            return colNames_[section] + " : " + colTypes_[section];
    } else if (o == Qt::Vertical && role == Qt::DisplayRole) {
        return QString::number((qlonglong)sourceRow(section));
    }
    return {};
}

bool ArrowTableModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid() || computing_ || !identity_) return false;
    return !fully_loaded_;
}

void ArrowTableModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || computing_ || fully_loaded_ || !identity_) return;
    int known = 0, now = 0;
    int64_t newrows = 0, tr = -1;
    withSource([&](TabularSource& s) {
        known = s.num_chunks();
        s.ensure(known);
        now = s.num_chunks();
        for (int c = 0; c < now; ++c) {
            ChunkMeta m = s.chunk_meta(c);
            newrows = std::max(newrows, m.first_row + m.num_rows);
        }
        tr = s.total_rows();
        return 0;
    });
    if (tr >= 0) { fully_loaded_ = true; newrows = tr; }
    else if (now == known) fully_loaded_ = true;
    refreshMeta();

    if (newrows > loaded_rows_) {
        beginInsertRows({}, (int)loaded_rows_, (int)newrows - 1);
        loaded_rows_ = newrows;
        endInsertRows();
    }
}

// Pure computation: order_ = filter (kept rows, source order) then sort within
// them. Polls `cancel` between the (coarse) phases; returns {} if aborted.
std::vector<int64_t> ArrowTableModel::computeOrderVec(
        TabularSource& src, std::vector<int> displayCols,
        FilterExpr filter, bool hasFilter, int sortCol, Qt::SortOrder order,
        std::atomic<bool>* cancel) {
    auto aborted = [&]{ return cancel && cancel->load(); };

    std::vector<int64_t> base;
    if (hasFilter) base = filter_rows(src, filter, cancel);   // source order
    if (aborted()) return {};

    if (sortCol >= 0 && sortCol < (int)displayCols.size()) {
        // The reader core's hand-rolled sort — the same one --sort uses —
        // because arrow::compute's sort_indices kernel is GC'd from the static
        // build and is not reliably registered in every Arrow linkage (using it
        // here made the click-to-sort a silent no-op). It reads only the sort
        // column, and pins a stream's batches so every row stays readable.
        std::vector<int64_t> out;
        if (!sort_rows_by_column(src, displayCols[sortCol], order == Qt::DescendingOrder,
                                 hasFilter ? &base : nullptr, cancel, &out))
            return {};
        return out;
    }
    // No sort (or sort failed): filtered rows in source order, else identity.
    return hasFilter ? base : std::vector<int64_t>{};
}

void ArrowTableModel::rebuildOrder() {
    auto v = withSource([&](TabularSource& s) {
        return computeOrderVec(s, displayCols_, filter_, hasFilter_, sortCol_, sortOrder_, nullptr);
    });
    identity_ = !hasFilter_ && v.empty();
    order_ = std::make_shared<std::vector<int64_t>>(std::move(v));
    refreshMeta();
}

void ArrowTableModel::sortByDisplayColumn(int displayCol, Qt::SortOrder order) {
    beginResetModel();
    if (displayCol < 0 || displayCol >= (int)displayCols_.size()) sortCol_ = -1;
    else { sortCol_ = displayCol; sortOrder_ = order; }
    rebuildOrder();
    clearChunkCache();
    invalidateFind();
    endResetModel();
}

void ArrowTableModel::setFilter(const FilterExpr& expr) {
    beginResetModel();
    filter_ = expr;
    hasFilter_ = true;
    rebuildOrder();
    clearChunkCache();
    invalidateFind();
    endResetModel();
}

void ArrowTableModel::clearFilter() {
    beginResetModel();
    hasFilter_ = false;
    filter_ = FilterExpr{};
    rebuildOrder();
    clearChunkCache();
    invalidateFind();
    endResetModel();
}

// ── Async filter/sort: compute order_ on a worker, swap in on completion ──────
void ArrowTableModel::setFilterAsync(const FilterExpr& expr) {
    filter_ = expr; hasFilter_ = true; startRecompute();
}
void ArrowTableModel::clearFilterAsync() {
    hasFilter_ = false; filter_ = FilterExpr{}; startRecompute();
}
void ArrowTableModel::sortAsyncByDisplayColumn(int displayCol, Qt::SortOrder order) {
    if (displayCol < 0 || displayCol >= (int)displayCols_.size()) sortCol_ = -1;
    else { sortCol_ = displayCol; sortOrder_ = order; }
    startRecompute();
}

void ArrowTableModel::startRecompute() {
    // Supersede any job in flight: its flag makes it stop at its next chunk
    // and jobId_ moves on, so its result is dropped. The new job queues behind
    // it on the owner thread. order_ is left intact (restored if this one is
    // canceled).
    if (cancel_) cancel_->store(true);
    beginResetModel();        // blank: rowCount()==0, data()=={}
    computing_ = true;
    endResetModel();
    emit recomputeStarted();

    auto cf = std::make_shared<std::atomic<bool>>(false);
    cancel_ = cf;
    const uint64_t id = ++jobId_;
    pendingJob_ = Job::Order;
    pendingHasFilter_ = hasFilter_;
    owner_->post([this, core = core_, mail = mail_, cols = displayCols_, f = filter_,
                  hf = hasFilter_, sc = sortCol_, so = sortOrder_, cf, id] {
        if (cf->load()) return;
        auto r = computeOrderVec(*core->src, cols, f, hf, sc, so, cf.get());
        if (cf->load()) return;
        Meta meta = readMeta(*core->src);
        deliver(mail, [this, id, r = std::move(r), meta]() mutable {
            onRecomputeDone(id, std::move(r), std::move(meta));
        });
    });
}

void ArrowTableModel::onRecomputeDone(uint64_t id, std::vector<int64_t> result, Meta meta) {
    if (!computing_ || id != jobId_) return;     // finalized, canceled or superseded
    if (cancel_ && cancel_->load()) return;
    meta_ = std::move(meta);
    if (pendingJob_ == Job::Find) {
        // Install the match list without disturbing the view (order_ unchanged):
        // unblank so rowCount()/data() serve the same rows as before the scan.
        findPos_     = std::move(result);
        findPattern_ = pendingRe_.pattern();
        findValid_   = true;
        beginResetModel();
        computing_  = false;
        pendingJob_ = Job::None;
        endResetModel();
        emit findReady((qint64)findPos_.size());
        return;
    }
    beginResetModel();
    identity_ = !pendingHasFilter_ && result.empty();
    order_ = std::make_shared<std::vector<int64_t>>(std::move(result));
    clearChunkCache();
    invalidateFind();                        // match positions are view-relative
    computing_ = false;
    pendingJob_ = Job::None;
    endResetModel();
    emit recomputeFinished((qint64)viewRows());
}

void ArrowTableModel::cancelRecompute() {
    if (!computing_) return;
    if (cancel_) cancel_->store(true);       // the job stops at its next chunk
    ++jobId_;                                // and its result is dropped
    beginResetModel();                       // restore the prior view (order_ unchanged)
    computing_ = false;
    pendingJob_ = Job::None;
    endResetModel();
    emit recomputeCanceled();
}

void ArrowTableModel::setSearch(const QRegularExpression& re) {
    searchRe_ = re;
    hasSearch_ = re.isValid() && !re.pattern().isEmpty();
    if (rowCount() > 0 && columnCount() > 0)
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
                         {Qt::BackgroundRole});
}

void ArrowTableModel::clearSearch() {
    hasSearch_ = false;
    invalidateFind();
    if (rowCount() > 0 && columnCount() > 0)
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
                         {Qt::BackgroundRole});
}

bool ArrowTableModel::findResultsValid(const QRegularExpression& re) const {
    return findValid_ && findPattern_ == re.pattern();
}

void ArrowTableModel::invalidateFind() {
    findValid_ = false;
    findPos_.clear();
    findPattern_.clear();
}

QModelIndex ArrowTableModel::findNext(const QModelIndex& from, bool forward) const {
    if (!hasSearch_) return {};
    int cols = columnCount();
    if (cols == 0) return {};
    // The synchronous fallback (self-test, or a stale list) computes the match
    // positions inline; the GUI normally primes them via findAsync first.
    if (!findResultsValid(searchRe_)) {
        const_cast<ArrowTableModel*>(this)->findPos_ = withSource([&](TabularSource& s) {
            return computeFindPos(s, displayCols_, order_, identity_, searchRe_, nullptr);
        });
        const_cast<ArrowTableModel*>(this)->findPattern_ = searchRe_.pattern();
        const_cast<ArrowTableModel*>(this)->findValid_ = true;
    }
    if (findPos_.empty()) return {};

    int startR = from.isValid() ? from.row() : 0;
    int startC = from.isValid() ? from.column() : -1;
    int64_t cur = (int64_t)startR * cols + startC;
    int64_t p;
    if (forward) {
        auto it = std::upper_bound(findPos_.begin(), findPos_.end(), cur);
        p = (it != findPos_.end()) ? *it : findPos_.front();   // wrap
    } else {
        auto it = std::lower_bound(findPos_.begin(), findPos_.end(), cur);
        p = (it != findPos_.begin()) ? *std::prev(it) : findPos_.back();  // wrap
    }
    int64_t r = p / cols, c = p % cols;
    if (r >= maxViewRows_) return {};   // past the rows the view can show
    return index((int)r, (int)c);
}

// Scan every (viewRow, col) for a regex match, returning the matching cell
// positions (viewRow*cols + col) sorted ascending. Iterates source chunks in
// order (cache-friendly), maps each source row back to its view row via the
// inverse of order_, and matches the Qt regex against each displayed cell.
// (An arrow::compute match_substring prefilter used to sit here, but its kernel
// is not registered in this Arrow linkage — the same reason the sort is
// hand-rolled — so it never skipped anything; the per-cell regex is the matcher.)
std::vector<int64_t>
ArrowTableModel::computeFindPos(TabularSource& src, std::vector<int> displayCols,
                               std::shared_ptr<const std::vector<int64_t>> order,
                               bool identity, QRegularExpression re,
                               std::atomic<bool>* cancel) {
    auto aborted = [&] { return cancel && cancel->load(); };
    const int cols = (int)displayCols.size();
    std::vector<int64_t> pos;
    if (cols == 0 || re.pattern().isEmpty()) return pos;

    // Map source row -> view row (identity: view == source).
    std::unordered_map<int64_t, int64_t> inv;
    if (!identity) {
        inv.reserve(order->size());
        for (int64_t v = 0; v < (int64_t)order->size(); ++v)
            inv.emplace((*order)[(size_t)v], v);
    }

    src.set_retain_all(true);   // a stream's batches: keep every one read
    drain(src);
    const int nch = src.num_chunks();
    for (int c = 0; c < nch; ++c) {
        if (aborted()) return {};
        std::shared_ptr<arrow::Table> tbl;
        if (!src.read_chunk(c, displayCols, &tbl).ok() || !tbl) continue;
        const int64_t first = src.chunk_meta(c).first_row;
        const int64_t nrows = tbl->num_rows();

        for (int64_t i = 0; i < nrows; ++i) {
            if ((i & 0x3fff) == 0 && aborted()) return {};
            const int64_t srcRow = first + i;
            int64_t viewRow;
            if (identity) {
                viewRow = srcRow;
            } else {
                auto it = inv.find(srcRow);
                if (it == inv.end()) continue;   // filtered out of the view
                viewRow = it->second;
            }
            for (int j = 0; j < cols; ++j) {
                std::string raw = chunked_cell(*tbl->column(j), i);
                raw = src.format_cell(displayCols[j], std::move(raw));
                if (re.match(QString::fromStdString(raw)).hasMatch())
                    pos.push_back(viewRow * (int64_t)cols + j);
            }
        }
    }
    if (aborted()) return {};
    std::sort(pos.begin(), pos.end());
    return pos;
}

void ArrowTableModel::findAsync(const QRegularExpression& re) {
    searchRe_  = re;
    hasSearch_ = re.isValid() && !re.pattern().isEmpty();
    if (!hasSearch_) { invalidateFind(); return; }

    if (cancel_) cancel_->store(true);   // supersede any job in flight
    beginResetModel();                   // blank while the owner scans
    computing_ = true;
    endResetModel();
    emit recomputeStarted();

    auto cf = std::make_shared<std::atomic<bool>>(false);
    cancel_     = cf;
    const uint64_t id = ++jobId_;
    pendingJob_ = Job::Find;
    pendingRe_  = re;
    owner_->post([this, core = core_, mail = mail_, cols = displayCols_, order = order_,
                  identity = identity_, re, cf, id] {
        if (cf->load()) return;
        auto r = computeFindPos(*core->src, cols, order, identity, re, cf.get());
        if (cf->load()) return;
        Meta meta = readMeta(*core->src);
        deliver(mail, [this, id, r = std::move(r), meta]() mutable {
            onRecomputeDone(id, std::move(r), std::move(meta));
        });
    });
}

bool ArrowTableModel::summarize(const std::vector<int>& displayCols,
                                std::vector<ColumnSummary>* out, QString* err) const {
    if (computing_) { *err = QStringLiteral("the table is busy"); return false; }
    SummaryOptions opt;
    for (int c : displayCols) {
        if (c < 0 || c >= (int)displayCols_.size()) { *err = QStringLiteral("no such column"); return false; }
        opt.cols.push_back(displayCols_[c]);
    }
    opt.filter       = hasFilter_ ? &filter_ : nullptr;
    opt.distinct_cap = 10000;
    opt.value_counts = true;
    const std::string e = withSource([&](TabularSource& s) {
        s.set_retain_all(true);     // the viewer re-reads every chunk afterwards
        return summarize_columns(s, opt, out);
    });
    if (!e.empty()) { *err = QString::fromStdString(e); return false; }
    return true;
}

bool ArrowTableModel::stepSlice(int delta) {
    if (computing_) return false;   // don't rebuild the source mid-recompute
    if (!withSource([&](TabularSource& s) { return s.change_slice(delta, /*absolute=*/false, 0); }))
        return false;
    beginResetModel();
    clearChunkCache(); chunkFirstRow_.clear();   // source rebuilt
    order_ = std::make_shared<std::vector<int64_t>>(); identity_ = true; sortCol_ = -1;
    hasFilter_ = false; filter_ = FilterExpr{};
    invalidateFind();
    schema_ = withSource([](TabularSource& s) { return s.schema(); });
    reseedRowCount();
    endResetModel();
    return true;
}

QString ArrowTableModel::footer() const {
    if (computing_) return QStringLiteral("Working…");
    QString f = QString::fromStdString(meta_.footer);
    // A stream that failed (a malformed record, a truncated file) holds only
    // the rows before the failure; say so rather than show them as the file.
    if (const arrow::Status& st = meta_.readStatus; !st.ok())
        f += QStringLiteral("  |  read error (rows after it are missing): %1")
                 .arg(QString::fromStdString(st.message()));
    if (const int64_t n = viewRows(); n > maxViewRows_)
        f += QStringLiteral("  |  the window shows the first %1 of %2 rows; an export writes all")
                 .arg(QLocale().toString((qlonglong)maxViewRows_))
                 .arg(QLocale().toString((qlonglong)n));
    return f;
}
QString ArrowTableModel::columnName(int c) const {
    return (c >= 0 && c < (int)colNames_.size()) ? colNames_[c] : QString();
}
QString ArrowTableModel::columnType(int c) const {
    return (c >= 0 && c < (int)colTypes_.size()) ? colTypes_[c] : QString();
}

std::vector<int> ArrowTableModel::selectColumns(const std::string& spec,
                                               std::vector<std::string>* unknown) const {
    return withSource([&](TabularSource& s) { return select_columns(s, spec, unknown); });
}
