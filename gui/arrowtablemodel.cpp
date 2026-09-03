#include "arrowtablemodel.h"

#include <QBrush>
#include <QColor>
#include <QString>
#include <QtConcurrent>
#include <algorithm>
#include <unordered_set>
#include <arrow/compute/api.h>

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

// Element i of a boolean ChunkedArray (a match_substring result). A null
// (e.g. a null cell) counts as "no match".
bool chunkedBoolAt(const arrow::ChunkedArray& ca, int64_t i) {
    for (const auto& a : ca.chunks()) {
        if (i < a->length()) {
            const auto& b = static_cast<const arrow::BooleanArray&>(*a);
            return b.IsValid(i) && b.Value(i);
        }
        i -= a->length();
    }
    return false;
}

// A pattern with no regex metacharacters is a plain substring needle.
bool isLiteralPattern(const QString& pat) {
    for (QChar ch : pat)
        if (QStringLiteral("\\^$.|?*+()[]{}").contains(ch)) return false;
    return true;
}
}  // namespace

ArrowTableModel::ArrowTableModel(std::unique_ptr<TabularSource> src,
                                 QObject* parent)
    : QAbstractTableModel(parent), src_(std::move(src)) {
    schema_ = src_->schema();

    auto hidden = src_->hidden_for_display();
    std::vector<std::string> hide(hidden.begin(), hidden.end());
    for (int i = 0; i < schema_->num_fields(); ++i) {
        const auto& f = schema_->field(i);
        bool is_hidden = false;
        for (const auto& h : hide) if (h == f->name()) { is_hidden = true; break; }
        if (is_hidden) continue;
        displayCols_.push_back(i);
        colNames_.push_back(QString::fromStdString(f->name()));
        colTypes_.push_back(QString::fromStdString(f->type()->ToString()));
    }
    watcher_ = new QFutureWatcher<std::vector<int64_t>>(this);
    connect(watcher_, &QFutureWatcher<std::vector<int64_t>>::finished,
            this, &ArrowTableModel::onRecomputeDone);
    reseedRowCount();
}

ArrowTableModel::~ArrowTableModel() {
    // The worker drives src_ (a member); make sure it has stopped before the
    // source is destroyed. cancel_ makes it bail at the next phase boundary.
    if (cancel_) cancel_->store(true);
    if (watcher_ && watcher_->isRunning()) watcher_->waitForFinished();
}

void ArrowTableModel::reseedRowCount() {
    loaded_rows_ = 0;
    fully_loaded_ = false;
    int64_t tr = src_->total_rows();
    if (tr >= 0) { loaded_rows_ = tr; fully_loaded_ = true; return; }
    src_->ensure(0);
    for (int c = 0; c < src_->num_chunks(); ++c) {
        ChunkMeta m = src_->chunk_meta(c);
        loaded_rows_ = std::max(loaded_rows_, m.first_row + m.num_rows);
    }
    if (src_->total_rows() >= 0) { loaded_rows_ = src_->total_rows(); fully_loaded_ = true; }
}

void ArrowTableModel::drainStreaming() const {
    if (src_->total_rows() >= 0) return;
    while (true) {
        int n = src_->num_chunks();
        src_->ensure(n);
        if (src_->num_chunks() == n) break;
    }
}

int64_t ArrowTableModel::sourceTotal() const {
    if (computing_) return loaded_rows_;   // don't touch src_ while a worker owns it
    drainStreaming();
    int64_t tr = src_->total_rows();
    if (tr >= 0) return tr;
    int64_t n = 0;
    for (int c = 0; c < src_->num_chunks(); ++c) {
        ChunkMeta m = src_->chunk_meta(c);
        n = std::max(n, m.first_row + m.num_rows);
    }
    return n;
}

int64_t ArrowTableModel::viewRows() const {
    if (!order_.empty()) return (int64_t)order_.size();
    return src_->total_rows() >= 0 ? src_->total_rows() : loaded_rows_;
}

int ArrowTableModel::chunkIndexForRow(int64_t row) const {
    if (row < 0) return -1;
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
    int c = chunkIndexForRow(row);
    if (c < 0) return nullptr;
    ChunkMeta m = src_->chunk_meta(c);
    auto it = cache_.find(c);
    if (it == cache_.end()) {
        std::shared_ptr<arrow::Table> tbl;
        if (!src_->read_chunk(c, displayCols_, &tbl).ok() || !tbl)
            return nullptr;
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
    return &it->second;
}

QString ArrowTableModel::cellText(int viewRow, int dispCol) const {
    const LoadedChunk* lc = chunkForRow(sourceRow(viewRow));
    if (!lc || !lc->table) return {};
    if (dispCol < 0 || dispCol >= lc->table->num_columns()) return {};
    int64_t local = sourceRow(viewRow) - lc->first_row;
    std::string raw = chunked_cell(*lc->table->column(dispCol), local);
    raw = src_->format_cell(displayCols_[dispCol], std::move(raw));
    return QString::fromStdString(raw);
}

QString ArrowTableModel::rawCellText(int viewRow, int dispCol) const {
    const LoadedChunk* lc = chunkForRow(sourceRow(viewRow));
    if (!lc || !lc->table) return {};
    if (dispCol < 0 || dispCol >= lc->table->num_columns()) return {};
    int64_t local = sourceRow(viewRow) - lc->first_row;
    std::string raw = chunked_cell_raw(*lc->table->column(dispCol), local);
    raw = src_->format_cell(displayCols_[dispCol], std::move(raw));
    return QString::fromStdString(raw);
}

int ArrowTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    if (computing_) return 0;   // blanked while a worker recomputes order_
    int64_t n = viewRows();
    return (n > 2'000'000'000LL) ? 2'000'000'000 : (int)n;
}

int ArrowTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return (int)displayCols_.size();
}

QVariant ArrowTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || computing_) return {};
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
        return cellText(index.row(), index.column());
    if (role == RawTextRole)
        return rawCellText(index.row(), index.column());
    if (role == Qt::BackgroundRole && hasSearch_) {
        QString v = cellText(index.row(), index.column());
        if (searchRe_.match(v).hasMatch())
            return QBrush(QColor(255, 235, 130));   // soft yellow highlight
    }
    return {};
}

QVariant ArrowTableModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o == Qt::Horizontal) {
        if (section < 0 || section >= (int)colNames_.size()) return {};
        if (role == Qt::DisplayRole)
            return colNames_[section] + "\n" + colTypes_[section];
        if (role == Qt::ToolTipRole)
            return colNames_[section] + " : " + colTypes_[section];
    } else if (o == Qt::Vertical && role == Qt::DisplayRole) {
        return QString::number((qlonglong)sourceRow(section));
    }
    return {};
}

bool ArrowTableModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid() || computing_ || !order_.empty()) return false;
    return !fully_loaded_;
}

void ArrowTableModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || computing_ || fully_loaded_ || !order_.empty()) return;
    int known = src_->num_chunks();
    src_->ensure(known);
    int now = src_->num_chunks();
    int64_t newrows = 0;
    for (int c = 0; c < now; ++c) {
        ChunkMeta m = src_->chunk_meta(c);
        newrows = std::max(newrows, m.first_row + m.num_rows);
    }
    int64_t tr = src_->total_rows();
    if (tr >= 0) { fully_loaded_ = true; newrows = tr; }
    else if (now == known) fully_loaded_ = true;

    if (newrows > loaded_rows_) {
        beginInsertRows({}, (int)loaded_rows_, (int)newrows - 1);
        loaded_rows_ = newrows;
        endInsertRows();
    }
}

std::shared_ptr<arrow::ChunkedArray>
ArrowTableModel::readFullColumn(int srcCol) const {
    drainStreaming();
    arrow::ArrayVector chunks;
    std::shared_ptr<arrow::DataType> type;
    for (int c = 0; c < src_->num_chunks(); ++c) {
        std::shared_ptr<arrow::Table> tbl;
        if (!src_->read_chunk(c, {srcCol}, &tbl).ok() || !tbl) continue;
        auto col = tbl->column(0);
        type = col->type();
        for (const auto& a : col->chunks()) chunks.push_back(a);
    }
    if (!type) return nullptr;
    return std::make_shared<arrow::ChunkedArray>(std::move(chunks), type);
}

// Pure computation: order_ = filter (kept rows, source order) then sort within
// them. Polls `cancel` between the (coarse) phases; returns {} if aborted.
std::vector<int64_t> ArrowTableModel::computeOrderVec(
        FilterExpr filter, bool hasFilter, int sortCol, Qt::SortOrder order,
        std::atomic<bool>* cancel) const {
    auto aborted = [&]{ return cancel && cancel->load(); };

    std::vector<int64_t> base;
    if (hasFilter) base = filter_rows(*src_, filter);     // source order
    if (aborted()) return {};

    if (sortCol >= 0 && sortCol < (int)displayCols_.size()) {
        auto ca = readFullColumn(displayCols_[sortCol]);
        if (aborted()) return {};
        if (ca && ca->length() > 0) {
            // Flatten the (possibly multi-chunk) column to one array so
            // stable_sort_order can index it. stable_sort_order is the reader
            // core's hand-rolled sort — the same one --sort uses — because
            // arrow::compute's sort_indices kernel is GC'd from the static build
            // and is not reliably registered in every Arrow linkage (using it
            // here made the click-to-sort a silent no-op: the header toggled but
            // the rows never moved).
            std::shared_ptr<arrow::Array> flat;
            if (ca->num_chunks() == 1) {
                flat = ca->chunk(0);
            } else {
                auto cc = arrow::Concatenate(ca->chunks());
                if (cc.ok()) flat = *cc;
            }
            if (flat) {
                std::vector<int64_t> idx =
                    stable_sort_order(*flat, order == Qt::DescendingOrder);
                if (aborted()) return {};
                if (hasFilter) {
                    std::unordered_set<int64_t> keep(base.begin(), base.end());
                    std::vector<int64_t> out;
                    out.reserve(keep.size());
                    for (size_t i = 0; i < idx.size(); ++i) {
                        if ((i & 0xffff) == 0 && aborted()) return {};
                        if (keep.count(idx[i])) out.push_back(idx[i]);
                    }
                    return out;
                }
                return idx;
            }
        }
    }
    // No sort (or sort failed): filtered rows in source order, else identity.
    return hasFilter ? base : std::vector<int64_t>{};
}

void ArrowTableModel::rebuildOrder() {
    order_ = computeOrderVec(filter_, hasFilter_, sortCol_, sortOrder_, nullptr);
}

void ArrowTableModel::sortByDisplayColumn(int displayCol, Qt::SortOrder order) {
    beginResetModel();
    if (displayCol < 0 || displayCol >= (int)displayCols_.size()) sortCol_ = -1;
    else { sortCol_ = displayCol; sortOrder_ = order; }
    rebuildOrder();
    cache_.clear(); lru_.clear();
    invalidateFind();
    endResetModel();
}

void ArrowTableModel::setFilter(const FilterExpr& expr) {
    beginResetModel();
    filter_ = expr;
    hasFilter_ = true;
    rebuildOrder();
    cache_.clear(); lru_.clear();
    invalidateFind();
    endResetModel();
}

void ArrowTableModel::clearFilter() {
    beginResetModel();
    hasFilter_ = false;
    filter_ = FilterExpr{};
    rebuildOrder();
    cache_.clear(); lru_.clear();
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
    // Supersede any in-flight job: flag its (shared) cancel so it bails; the
    // watcher is repointed at the new future below, so the orphan's result is
    // ignored. order_ is left intact (restored if this one is canceled).
    if (cancel_) cancel_->store(true);
    beginResetModel();        // blank: rowCount()==0, data()=={} → UI leaves src_ alone
    computing_ = true;
    endResetModel();
    emit recomputeStarted();

    auto cf = std::make_shared<std::atomic<bool>>(false);
    cancel_ = cf;
    pendingJob_ = Job::Order;
    FilterExpr  fsnap = filter_;
    bool        hf    = hasFilter_;
    int         sc    = sortCol_;
    Qt::SortOrder so  = sortOrder_;
    watcher_->setFuture(QtConcurrent::run(
        [this, fsnap, hf, sc, so, cf] {
            return computeOrderVec(fsnap, hf, sc, so, cf.get());
        }));
}

void ArrowTableModel::onRecomputeDone() {
    if (!computing_) return;                 // already finalized or canceled
    if (cancel_ && cancel_->load()) return;  // superseded/canceled — ignore
    if (pendingJob_ == Job::Find) {
        // Install the match list without disturbing the view (order_ unchanged):
        // unblank so rowCount()/data() serve the same rows as before the scan.
        findPos_     = watcher_->result();
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
    order_ = watcher_->result();
    cache_.clear(); lru_.clear();
    invalidateFind();                        // match positions are view-relative
    computing_ = false;
    pendingJob_ = Job::None;
    endResetModel();
    emit recomputeFinished((qint64)viewRows());
}

void ArrowTableModel::cancelRecompute() {
    if (!computing_) return;
    if (cancel_) cancel_->store(true);       // worker bails at next phase
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
        const_cast<ArrowTableModel*>(this)->findPos_ =
            computeFindPos(searchRe_, nullptr);
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
    if (r > 2'000'000'000LL) return {};   // past Qt's int row cap (documented)
    return index((int)r, (int)c);
}

// Scan every (viewRow, col) for a regex match, returning the matching cell
// positions (viewRow*cols + col) sorted ascending. Iterates source chunks in
// order (cache-friendly), maps each source row back to its view row via the
// inverse of order_, and for literal needles on Arrow string columns uses
// match_substring as a candidate prefilter — the Qt regex on the displayed
// cell text is always the source of truth, so the result equals a pure
// per-cell scan (the prefilter only skips cells Arrow rules out; this is exact
// for ASCII needles, the realistic case).
std::vector<int64_t>
ArrowTableModel::computeFindPos(QRegularExpression re,
                               std::atomic<bool>* cancel) const {
    auto aborted = [&] { return cancel && cancel->load(); };
    const int cols = (int)displayCols_.size();
    std::vector<int64_t> pos;
    if (cols == 0 || re.pattern().isEmpty()) return pos;

    // A literal needle (no regex metacharacters) can be vectorized with
    // match_substring on string columns.
    const QString pat = re.pattern();
    const bool literal = isLiteralPattern(pat);
    const std::string needle = literal ? pat.toStdString() : std::string();
    const bool ci = re.patternOptions() & QRegularExpression::CaseInsensitiveOption;

    std::vector<char> strCol(cols, 0);
    for (int j = 0; j < cols; ++j) {
        auto id = schema_->field(displayCols_[j])->type()->id();
        strCol[j] = (id == arrow::Type::STRING || id == arrow::Type::LARGE_STRING);
    }

    // Map source row -> view row. Empty order_ == identity (view == source).
    const bool identity = order_.empty();
    std::unordered_map<int64_t, int64_t> inv;
    if (!identity) {
        inv.reserve(order_.size());
        for (int64_t v = 0; v < (int64_t)order_.size(); ++v)
            inv.emplace(order_[v], v);
    }

    drainStreaming();
    const int nch = src_->num_chunks();
    for (int c = 0; c < nch; ++c) {
        if (aborted()) return {};
        std::shared_ptr<arrow::Table> tbl;
        if (!src_->read_chunk(c, displayCols_, &tbl).ok() || !tbl) continue;
        const int64_t first = src_->chunk_meta(c).first_row;
        const int64_t nrows = tbl->num_rows();

        // Per-column candidate masks (literal needle, string columns only).
        std::vector<std::shared_ptr<arrow::ChunkedArray>> mask(cols);
        if (literal) {
            arrow::compute::MatchSubstringOptions opt(needle, ci);
            for (int j = 0; j < cols; ++j) {
                if (!strCol[j]) continue;
                auto r = arrow::compute::CallFunction(
                    "match_substring", {arrow::Datum(tbl->column(j))}, &opt);
                if (r.ok()) mask[j] = r->chunked_array();
            }
        }

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
                // Arrow candidate prefilter: skip cells the substring match
                // rules out; everything else is checked by the Qt regex.
                if (mask[j] && !chunkedBoolAt(*mask[j], i)) continue;
                std::string raw = chunked_cell(*tbl->column(j), i);
                raw = src_->format_cell(displayCols_[j], std::move(raw));
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

    if (cancel_) cancel_->store(true);   // supersede any in-flight worker
    beginResetModel();                   // blank → UI leaves src_ to the worker
    computing_ = true;
    endResetModel();
    emit recomputeStarted();

    auto cf = std::make_shared<std::atomic<bool>>(false);
    cancel_     = cf;
    pendingJob_ = Job::Find;
    pendingRe_  = re;
    QRegularExpression resnap = re;
    watcher_->setFuture(QtConcurrent::run(
        [this, resnap, cf] { return computeFindPos(resnap, cf.get()); }));
}

ColStats ArrowTableModel::columnStats(int displayCol) const {
    if (computing_) return {};   // would drain src_ while a worker owns it
    if (displayCol < 0 || displayCol >= (int)displayCols_.size()) return {};
    return compute_col_stats(*src_, displayCols_[displayCol]);
}

bool ArrowTableModel::stepSlice(int delta) {
    if (computing_) return false;   // don't rebuild the source mid-recompute
    if (!src_->change_slice(delta, /*absolute=*/false, 0)) return false;
    beginResetModel();
    cache_.clear(); lru_.clear(); chunkFirstRow_.clear();   // source rebuilt
    order_.clear(); sortCol_ = -1;
    hasFilter_ = false; filter_ = FilterExpr{};
    invalidateFind();
    schema_ = src_->schema();
    reseedRowCount();
    endResetModel();
    return true;
}

QString ArrowTableModel::footer() const {
    if (computing_) return QStringLiteral("Working…");
    return QString::fromStdString(src_->footer());
}
QString ArrowTableModel::columnName(int c) const {
    return (c >= 0 && c < (int)colNames_.size()) ? colNames_[c] : QString();
}
QString ArrowTableModel::columnType(int c) const {
    return (c >= 0 && c < (int)colTypes_.size()) ? colTypes_[c] : QString();
}
