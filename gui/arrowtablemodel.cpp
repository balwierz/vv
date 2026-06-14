#include "arrowtablemodel.h"

#include <QBrush>
#include <QColor>
#include <QString>
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
    reseedRowCount();
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

int ArrowTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    int64_t n = viewRows();
    return (n > 2'000'000'000LL) ? 2'000'000'000 : (int)n;
}

int ArrowTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return (int)displayCols_.size();
}

QVariant ArrowTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
        return cellText(index.row(), index.column());
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
    if (parent.isValid() || !order_.empty()) return false;
    return !fully_loaded_;
}

void ArrowTableModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid() || fully_loaded_ || !order_.empty()) return;
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

// Recompute order_ = filter (kept rows, source order) then sort within them.
void ArrowTableModel::rebuildOrder() {
    std::vector<int64_t> base;
    if (hasFilter_) base = filter_rows(*src_, filter_);   // source order

    if (sortCol_ >= 0 && sortCol_ < (int)displayCols_.size()) {
        auto ca = readFullColumn(displayCols_[sortCol_]);
        if (ca && ca->length() > 0) {
            arrow::compute::SortOptions opts(
                {arrow::compute::SortKey(
                    "", sortOrder_ == Qt::AscendingOrder
                            ? arrow::compute::SortOrder::Ascending
                            : arrow::compute::SortOrder::Descending)});
            auto res = arrow::compute::CallFunction("sort_indices",
                                                    {arrow::Datum(ca)}, &opts);
            if (res.ok()) {
                auto idx = std::static_pointer_cast<arrow::UInt64Array>(
                    res->make_array());
                if (hasFilter_) {
                    std::unordered_set<int64_t> keep(base.begin(), base.end());
                    order_.clear();
                    order_.reserve(keep.size());
                    for (int64_t i = 0; i < idx->length(); ++i) {
                        int64_t r = (int64_t)idx->Value(i);
                        if (keep.count(r)) order_.push_back(r);
                    }
                } else {
                    order_.resize(idx->length());
                    for (int64_t i = 0; i < idx->length(); ++i)
                        order_[i] = (int64_t)idx->Value(i);
                }
                return;
            }
        }
    }
    // No sort (or sort failed): filtered rows in source order, else identity.
    order_ = hasFilter_ ? std::move(base) : std::vector<int64_t>{};
}

void ArrowTableModel::sortByDisplayColumn(int displayCol, Qt::SortOrder order) {
    beginResetModel();
    if (displayCol < 0 || displayCol >= (int)displayCols_.size()) sortCol_ = -1;
    else { sortCol_ = displayCol; sortOrder_ = order; }
    rebuildOrder();
    cache_.clear(); lru_.clear();
    endResetModel();
}

void ArrowTableModel::setFilter(const FilterExpr& expr) {
    beginResetModel();
    filter_ = expr;
    hasFilter_ = true;
    rebuildOrder();
    cache_.clear(); lru_.clear();
    endResetModel();
}

void ArrowTableModel::clearFilter() {
    beginResetModel();
    hasFilter_ = false;
    filter_ = FilterExpr{};
    rebuildOrder();
    cache_.clear(); lru_.clear();
    endResetModel();
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
    if (rowCount() > 0 && columnCount() > 0)
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
                         {Qt::BackgroundRole});
}

QModelIndex ArrowTableModel::findNext(const QModelIndex& from, bool forward) const {
    if (!hasSearch_) return {};
    int rows = rowCount(), cols = columnCount();
    if (rows == 0 || cols == 0) return {};
    int startR = from.isValid() ? from.row() : 0;
    int startC = from.isValid() ? from.column() : -1;
    int64_t total = (int64_t)rows * cols;
    int64_t pos = (int64_t)startR * cols + startC;
    for (int64_t step = 1; step <= total; ++step) {
        int64_t p = forward ? (pos + step) % total
                            : (pos - step + total) % total;
        int r = (int)(p / cols), c = (int)(p % cols);
        if (searchRe_.match(cellText(r, c)).hasMatch())
            return index(r, c);
    }
    return {};
}

ColStats ArrowTableModel::columnStats(int displayCol) const {
    if (displayCol < 0 || displayCol >= (int)displayCols_.size()) return {};
    return compute_col_stats(*src_, displayCols_[displayCol]);
}

bool ArrowTableModel::stepSlice(int delta) {
    if (!src_->change_slice(delta, /*absolute=*/false, 0)) return false;
    beginResetModel();
    cache_.clear(); lru_.clear(); chunkFirstRow_.clear();   // source rebuilt
    order_.clear(); sortCol_ = -1;
    hasFilter_ = false; filter_ = FilterExpr{};
    schema_ = src_->schema();
    reseedRowCount();
    endResetModel();
    return true;
}

QString ArrowTableModel::footer() const {
    return QString::fromStdString(src_->footer());
}
QString ArrowTableModel::columnName(int c) const {
    return (c >= 0 && c < (int)colNames_.size()) ? colNames_[c] : QString();
}
QString ArrowTableModel::columnType(int c) const {
    return (c >= 0 && c < (int)colTypes_.size()) ? colTypes_[c] : QString();
}
