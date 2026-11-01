#include "columnwidths.h"

#include "arrowtablemodel.h"
#include "sequencedelegate.h"
#include "typedheader.h"

#include <QBitArray>
#include <QFontDatabase>
#include <QHeaderView>
#include <QTableView>

#include <algorithm>

// Auto-size columns to their content at open. Each column is sized to the
// 95th percentile of a sample of its rendered cells, so the common case
// shows in full and the occasional long value elides; capped so a prose
// column can't dominate. The horizontal scrollbar absorbs any overflow, so
// identifiers are always shown in full rather than pre-truncated. This runs
// once at open; nothing resizes the columns afterwards, so a manual drag is
// preserved. A windowed tab (ArrowTableModel::columnWindowed) plans only the
// columns of its window: the others get -1 (see autosizeColumns).
std::vector<int> planColumnWidths(ArrowTableModel* model, const QFontMetrics& fm,
                                  const QFontMetrics* typeFm) {
    // A summary tab (an AnnData's key / value / about) shows its cells in
    // full, as the CLI table does.
    const bool full = model->tabTraits().cells_in_full;
    const int ncols = model->columnCount();
    const int64_t nrows = model->rowCount();
    if (ncols <= 0) return {};
    std::vector<int> cols;
    if (model->columnWindowed()) cols = model->columnWindow();
    else for (int c = 0; c < ncols; ++c) cols.push_back(c);
    const int ch  = std::max(1, fm.horizontalAdvance(QLatin1Char('0')));
    const int pad = 2 * ch;                 // cell text margins, both sides
    // About 40,000 cells, 64 to 2,000 rows, of the rows already loaded:
    // every cell sampled is measured on the UI thread, and a row not yet
    // read would only show "…".
    const int64_t sample = std::min<int64_t>({nrows, (int64_t)std::clamp(40000 / std::max<int>(1, (int)cols.size()), 64, 2000),
                                              model->loadedPrefixRows()});

    std::vector<std::vector<int>> samples(cols.size());
    std::vector<int> floors(cols.size());
    for (size_t k = 0; k < cols.size(); ++k) {
        const int c = cols[k];
        floors[k] = columnHeaderFloor(model, c, fm, typeFm);
        samples[k].reserve((size_t)sample);
        for (int64_t r = 0; r < sample; ++r) {
            QString t = model->data(model->index((int)r, c), Qt::DisplayRole).toString();
            if (t.isEmpty() || t == QStringLiteral("\u2026")) continue;   // nulls / blanks / unread don't set width
            samples[k].push_back(fm.horizontalAdvance(t) + pad);
        }
    }

    WidthPlanOptions opt;
    opt.percentile = 95;
    opt.c_max      = full ? (1 << 20) : 50 * ch + pad;   // 50 characters, in pixels
    opt.slack      = 2 * ch;
    opt.min_floor  = 3 * ch;
    // viewport 0 = no shrink-to-fit: give each column its natural width and
    // let the scrollbar handle a wide table (idiomatic for a GUI, and it
    // keeps identifiers un-truncated).
    const std::vector<int> planned = plan_column_widths(samples, floors, 0, opt);
    std::vector<int> w((size_t)ncols, -1);
    for (size_t k = 0; k < cols.size() && k < planned.size(); ++k) w[(size_t)cols[k]] = planned[k];
    return w;
}

int columnHeaderFloor(ArrowTableModel* model, int c, const QFontMetrics& fm, const QFontMetrics* typeFm) {
    // The header label must stay readable, plus room for the sort-indicator
    // arrow.
    const int ch  = std::max(1, fm.horizontalAdvance(QLatin1Char('0')));
    int headW = fm.horizontalAdvance(model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString());
    if (typeFm)
        headW = std::max(headW, typeFm->horizontalAdvance(
            model->headerData(c, Qt::Horizontal, ArrowTableModel::TypeRole).toString()));
    return headW + 2 * ch + 3 * ch;
}

// A windowed tab's columns past the ones sized at open: each is given, the
// first time it enters the window, the default width or its header's floor,
// whichever is wider. "vvSized" (a QBitArray on the view) marks the columns
// sized, so a manual drag is never undone.
void sizeWindowColumns(QTableView* view, ArrowTableModel* model) {
    if (!model->columnWindowed()) return;
    QBitArray sized = view->property("vvSized").toBitArray();
    if (sized.size() != model->columnCount()) return;    // not autosized yet
    auto* th = dynamic_cast<TypedHeaderView*>(view->horizontalHeader());
    const QFontMetrics typeFm(th ? th->typeFont() : view->font());
    const QFontMetrics* tfm = th && th->showTypes() ? &typeFm : nullptr;
    QHeaderView* hh = view->horizontalHeader();
    bool changed = false;
    for (int c : model->columnWindow()) {
        if (c >= sized.size() || sized.testBit(c)) continue;
        sized.setBit(c);
        changed = true;
        const int w = std::max(hh->defaultSectionSize(), columnHeaderFloor(model, c, view->fontMetrics(), tfm));
        if (w != hh->sectionSize(c)) hh->resizeSection(c, w);
    }
    if (changed) view->setProperty("vvSized", sized);
}

void autosizeColumns(QTableView* view, ArrowTableModel* model) {
    auto* th = dynamic_cast<TypedHeaderView*>(view->horizontalHeader());
    const QFontMetrics typeFm(th ? th->typeFont() : view->font());
    std::vector<int> w = planColumnWidths(model, view->fontMetrics(),
                                          th && th->showTypes() ? &typeFm : nullptr);
    QHeaderView* hh = view->horizontalHeader();
    // A column drawn as bases / qualities uses a fixed-width font: sized
    // by its longest sampled value in that font, up to 50 characters.
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(view->font().pointSizeF() > 0 ? view->font().pointSizeF() : mono.pointSizeF());
    const int cw = std::max(1, QFontMetrics(mono).horizontalAdvance(QLatin1Char('A')));
    // A windowed tab: the columns not planned take the widest planned data
    // column's width (a matrix's genes share one type) as the header's
    // default, so 62,710 columns are not resized one by one; the rest are
    // sized as they enter the window (sizeWindowColumns).
    if (model->columnWindowed()) {
        int def = 0;
        for (int c = 1; c < (int)w.size(); ++c) def = std::max(def, w[(size_t)c]);
        if (def > 0) hh->setDefaultSectionSize(def);
        QBitArray sized(model->columnCount());
        for (int c = 0; c < (int)w.size(); ++c) if (w[(size_t)c] >= 0) sized.setBit(c);
        view->setProperty("vvSized", sized);
    }
    for (int c = 0; c < model->columnCount() && c < (int)w.size(); ++c) {
        if (w[(size_t)c] < 0) continue;                  // windowed: not planned
        if (auto* sd = dynamic_cast<SequenceDelegate*>(view->itemDelegateForColumn(c))) {
            int longest = 0;
            const int rows = (int)std::min<int64_t>(model->loadedPrefixRows(), 2000);
            for (int r = 0; r < rows; ++r)
                longest = std::max(longest, (int)model->data(model->index(r, c), Qt::DisplayRole).toString().size());
            // An alignment is read across its columns: room for more of it.
            w[(size_t)c] = std::max(w[(size_t)c], std::min(longest, sd->isAlignment() ? 120 : 50) * cw + 2 * cw);
        }
        hh->resizeSection(c, w[(size_t)c]);
    }
}

