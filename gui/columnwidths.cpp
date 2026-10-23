#include "columnwidths.h"

#include "arrowtablemodel.h"
#include "sequencedelegate.h"
#include "typedheader.h"

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
// preserved.
std::vector<int> planColumnWidths(ArrowTableModel* model, const QFontMetrics& fm,
                                  const QFontMetrics* typeFm) {
    // A summary tab (an AnnData's key / value / about) shows its cells in
    // full, as the CLI table does.
    const bool full = model->tabTraits().cells_in_full;
    const int ncols = model->columnCount();
    const int64_t nrows = model->rowCount();
    if (ncols <= 0) return {};
    const int ch  = std::max(1, fm.horizontalAdvance(QLatin1Char('0')));
    const int pad = 2 * ch;                 // cell text margins, both sides
    // About 40,000 cells, 64 to 2,000 rows, of the rows already loaded:
    // every cell sampled is measured on the UI thread, and a row not yet
    // read would only show "…".
    const int64_t sample = std::min<int64_t>({nrows, (int64_t)std::clamp(40000 / ncols, 64, 2000),
                                              model->loadedPrefixRows()});

    std::vector<std::vector<int>> samples((size_t)ncols);
    std::vector<int> floors((size_t)ncols);
    for (int c = 0; c < ncols; ++c) {
        // Floor: the header label must stay readable, plus room for the
        // sort-indicator arrow.
        const QString head = model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        int headW = fm.horizontalAdvance(head);
        if (typeFm)
            headW = std::max(headW, typeFm->horizontalAdvance(
                model->headerData(c, Qt::Horizontal, ArrowTableModel::TypeRole).toString()));
        floors[(size_t)c] = headW + pad + 3 * ch;
        samples[(size_t)c].reserve((size_t)sample);
        for (int64_t r = 0; r < sample; ++r) {
            QString t = model->data(model->index((int)r, c), Qt::DisplayRole).toString();
            if (t.isEmpty()) continue;       // nulls / blanks don't set width
            samples[(size_t)c].push_back(fm.horizontalAdvance(t) + pad);
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
    return plan_column_widths(samples, floors, 0, opt);
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
    for (int c = 0; c < model->columnCount() && c < (int)w.size(); ++c) {
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

