// vvg — the Qt6 graphical frontend for vv.
//
// Stage 2 + 2b: open a file through libvvcore's open_source(), expand any
// sibling tabs (sheets/tables/datasets), and present each in a QTableView
// inside a QTabWidget. Feature parity with the TUI: click-to-sort (typed,
// via the model), a detail dock (all columns of the current row), copy
// selection as TSV, and slice stepping for NPZ 3-D+ arrays.
//
// A headless self-check (VVG_SELFTEST=1) prints row/column/tab counts and
// the first row, then exits — lets CI validate the in-process core path
// without a display server.

#include <QApplication>
#include <QClipboard>
#include <QDockWidget>
#include <QAction>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMap>
#include <QMessageBox>
#include <QRegularExpression>
#include <QShortcut>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QToolBar>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "arrowtablemodel.h"
#include "vv/vvcore.hpp"

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(std::vector<std::unique_ptr<TabularSource>> sources) {
        tabs_ = new QTabWidget(this);
        setCentralWidget(tabs_);

        for (auto& src : sources) {
            auto* model = new ArrowTableModel(std::move(src), this);
            auto* view  = new QTableView(tabs_);
            view->setModel(model);
            view->setAlternatingRowColors(true);
            view->setEditTriggers(QAbstractItemView::NoEditTriggers);
            view->setSelectionBehavior(QAbstractItemView::SelectItems);
            view->horizontalHeader()->setSectionsClickable(true);
            view->verticalHeader()->setDefaultSectionSize(
                view->fontMetrics().height() + 6);

            // Click a column header to sort (toggle asc/desc), typed via Arrow.
            connect(view->horizontalHeader(), &QHeaderView::sectionClicked,
                    this, [this, view, model](int section) {
                        Qt::SortOrder ord = Qt::AscendingOrder;
                        if (model->sortColumn() == section &&
                            sortOrder_.value(model, Qt::AscendingOrder) == Qt::AscendingOrder)
                            ord = Qt::DescendingOrder;
                        sortOrder_[model] = ord;
                        model->sortByDisplayColumn(section, ord);
                        view->horizontalHeader()->setSortIndicatorShown(true);
                        view->horizontalHeader()->setSortIndicator(section, ord);
                    });

            connect(view->selectionModel(), &QItemSelectionModel::currentChanged,
                    this, [this](const QModelIndex& cur, const QModelIndex&) {
                        updateDetail(cur);
                    });

            views_.push_back(view);
            models_.push_back(model);
            tabs_->addTab(view, QString::fromStdString(src_label(model)));
        }

        connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
            refreshStatus();
            if (auto* v = activeView())
                updateDetail(v->selectionModel()->currentIndex());
        });

        buildDetailDock();
        buildToolbar();
        buildSearchBar();
        new QShortcut(QKeySequence::Copy, this, [this]{ copySelection(); });
        new QShortcut(QKeySequence::FindNext, this, [this]{ doFind(true); });

        resize(1100, 760);
        refreshStatus();
    }

private:
    static std::string src_label(ArrowTableModel* m) {
        return m->source()->tab_label();
    }
    QTableView*     activeView()  const {
        return views_.empty() ? nullptr
                              : views_[std::max(0, tabs_->currentIndex())];
    }
    ArrowTableModel* activeModel() const {
        return models_.empty() ? nullptr
                               : models_[std::max(0, tabs_->currentIndex())];
    }

    void buildDetailDock() {
        auto* dock = new QDockWidget(tr("Row detail"), this);
        detail_ = new QTableWidget(0, 2, dock);
        detail_->setHorizontalHeaderLabels({tr("Column"), tr("Value")});
        detail_->horizontalHeader()->setStretchLastSection(true);
        detail_->verticalHeader()->setVisible(false);
        detail_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        dock->setWidget(detail_);
        addDockWidget(Qt::RightDockWidgetArea, dock);
    }

    void buildToolbar() {
        auto* tb = addToolBar(tr("View"));
        QAction* prev = tb->addAction(tr("◀ Slice"));
        connect(prev, &QAction::triggered, this, [this]{ stepSlice(-1); });
        QAction* next = tb->addAction(tr("Slice ▶"));
        connect(next, &QAction::triggered, this, [this]{ stepSlice(+1); });
        tb->addSeparator();
        QAction* clr = tb->addAction(tr("Clear sort"));
        connect(clr, &QAction::triggered, this, [this]{
            if (auto* m = activeModel()) {
                m->sortByDisplayColumn(-1, Qt::AscendingOrder);
                if (auto* v = activeView())
                    v->horizontalHeader()->setSortIndicatorShown(false);
            }
        });
        QAction* st = tb->addAction(tr("Σ Stats"));
        connect(st, &QAction::triggered, this, [this]{ showStats(); });
    }

    void showStats() {
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v) return;
        int col = v->currentIndex().isValid() ? v->currentIndex().column() : 0;
        ColStats s = m->columnStats(col);
        if (!s.valid) return;
        QString t = tr("Column: %1\nType: %2\nCount: %3   Nulls: %4\n")
            .arg(QString::fromStdString(s.name), QString::fromStdString(s.type))
            .arg((qlonglong)s.count).arg((qlonglong)s.nulls);
        if (s.is_numeric && s.count > 0)
            t += tr("Min: %1   Max: %2   Mean: %3\n")
                     .arg(s.min).arg(s.max).arg(s.mean, 0, 'g', 6);
        else if (s.count > 0)
            t += tr("Min: %1   Max: %2\n")
                     .arg(QString::fromStdString(s.s_min),
                          QString::fromStdString(s.s_max));
        if (s.distinct_overflow)
            t += tr("Distinct: > 16\n");
        else if (!s.distinct.empty()) {
            QStringList ds;
            for (const auto& d : s.distinct) ds << QString::fromStdString(d);
            t += tr("Distinct (%1): %2\n").arg(ds.size()).arg(ds.join(", "));
        }
        QMessageBox::information(this, tr("Column statistics — %1")
                                 .arg(QString::fromStdString(s.name)), t);
    }

    void buildSearchBar() {
        auto* tb = addToolBar(tr("Find / Filter"));
        tb->addWidget(new QLabel(tr("  Filter: ")));
        filterEdit_ = new QLineEdit(tb);
        filterEdit_->setPlaceholderText(tr("e.g.  score > 5 and chrom == \"chr1\""));
        filterEdit_->setClearButtonEnabled(true);
        filterEdit_->setMinimumWidth(280);
        tb->addWidget(filterEdit_);
        connect(filterEdit_, &QLineEdit::returnPressed, this, [this]{ applyFilter(); });

        tb->addWidget(new QLabel(tr("   Find: ")));
        findEdit_ = new QLineEdit(tb);
        findEdit_->setPlaceholderText(tr("regex (Enter = next)"));
        findEdit_->setClearButtonEnabled(true);
        findEdit_->setMinimumWidth(200);
        tb->addWidget(findEdit_);
        connect(findEdit_, &QLineEdit::returnPressed, this, [this]{ doFind(true); });
        connect(findEdit_, &QLineEdit::textChanged, this, [this](const QString& t){
            if (auto* m = activeModel()) {
                if (t.isEmpty()) m->clearSearch();
                else m->setSearch(QRegularExpression(
                         t, QRegularExpression::CaseInsensitiveOption));
            }
        });
    }

    void applyFilter() {
        auto* m = activeModel();
        if (!m) return;
        QString text = filterEdit_->text().trimmed();
        if (text.isEmpty()) {
            m->clearFilter();
            filterEdit_->setStyleSheet({});
            refreshStatus();
            return;
        }
        FilterExpr fx;
        std::string err;
        if (!parse_filter_expr(text.toStdString(), *m->source()->schema(),
                               &fx, &err)) {
            filterEdit_->setStyleSheet("background:#ffd6d6;");
            statusBar()->showMessage(tr("filter: %1").arg(QString::fromStdString(err)));
            return;
        }
        filterEdit_->setStyleSheet({});
        m->setFilter(fx);
        statusBar()->showMessage(tr("filter: %1 / %2 rows  —  %3")
            .arg((qlonglong)m->viewRows())
            .arg((qlonglong)m->sourceTotal())
            .arg(m->footer()));
    }

    void doFind(bool forward) {
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v) return;
        QString pat = findEdit_->text();
        if (pat.isEmpty()) { m->clearSearch(); return; }
        m->setSearch(QRegularExpression(pat, QRegularExpression::CaseInsensitiveOption));
        QModelIndex hit = m->findNext(v->currentIndex(), forward);
        if (hit.isValid()) {
            v->setCurrentIndex(hit);
            v->scrollTo(hit, QAbstractItemView::PositionAtCenter);
        } else {
            statusBar()->showMessage(tr("find: no match"), 2000);
        }
    }

    void stepSlice(int delta) {
        if (auto* m = activeModel()) {
            if (m->stepSlice(delta)) refreshStatus();
        }
    }

    void updateDetail(const QModelIndex& cur) {
        if (!detail_) return;
        auto* m = activeModel();
        if (!m || !cur.isValid()) { detail_->setRowCount(0); return; }
        int cols = m->displayColumnCount();
        detail_->setRowCount(cols);
        for (int c = 0; c < cols; ++c) {
            detail_->setItem(c, 0, new QTableWidgetItem(m->columnName(c)));
            QString v = m->data(m->index(cur.row(), c), Qt::DisplayRole).toString();
            detail_->setItem(c, 1, new QTableWidgetItem(v));
        }
        detail_->resizeColumnToContents(0);
    }

    void copySelection() {
        auto* v = activeView();
        if (!v) return;
        auto sel = v->selectionModel()->selectedIndexes();
        if (sel.isEmpty()) return;
        std::sort(sel.begin(), sel.end(), [](const QModelIndex& a, const QModelIndex& b){
            return a.row() != b.row() ? a.row() < b.row() : a.column() < b.column();
        });
        QString out;
        int curRow = sel.first().row();
        bool firstInRow = true;
        for (const auto& idx : sel) {
            if (idx.row() != curRow) { out += '\n'; curRow = idx.row(); firstInRow = true; }
            if (!firstInRow) out += '\t';
            out += idx.data(Qt::DisplayRole).toString();
            firstInRow = false;
        }
        QApplication::clipboard()->setText(out);
        statusBar()->showMessage(tr("Copied %1 cell(s)").arg(sel.size()), 2000);
    }

    void refreshStatus() {
        if (auto* m = activeModel())
            statusBar()->showMessage(m->footer());
    }

    QTabWidget*                    tabs_   = nullptr;
    QTableWidget*                  detail_ = nullptr;
    QLineEdit*                     filterEdit_ = nullptr;
    QLineEdit*                     findEdit_   = nullptr;
    std::vector<QTableView*>       views_;
    std::vector<ArrowTableModel*>  models_;
    QMap<ArrowTableModel*, Qt::SortOrder> sortOrder_;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    std::string path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (!a.empty() && a[0] != '-') { path = a; break; }
    }
    if (path.empty()) { std::fprintf(stderr, "usage: vvg <file>\n"); return 2; }

    Config cfg;
    cfg.path = path;
    std::unique_ptr<TabularSource> src;
    std::string err = open_source(path, cfg, &src);
    if (!err.empty()) { std::fprintf(stderr, "vvg: %s\n", err.c_str()); return 1; }

    // Gather the first source plus any sibling tabs.
    std::vector<std::unique_ptr<TabularSource>> sources;
    auto siblings = src->expand_tabs();
    sources.push_back(std::move(src));
    for (auto& s : siblings) sources.push_back(std::move(s));

    if (const char* st = std::getenv("VVG_SELFTEST"); st && *st && *st != '0') {
        ArrowTableModel m(std::move(sources.front()));
        std::printf("tabs=%zu rows=%d cols=%d\n", sources.size(),
                    m.rowCount(), m.columnCount());
        std::printf("footer=%s\n", m.footer().toStdString().c_str());
        if (m.columnCount() > 0) {
            // Exercise the typed sort path on column 0.
            m.sortByDisplayColumn(0, Qt::AscendingOrder);
            std::string r0;
            for (int c = 0; c < m.columnCount(); ++c) {
                if (c) r0 += " | ";
                r0 += m.data(m.index(0, c), Qt::DisplayRole).toString().toStdString();
            }
            std::printf("sorted_row0=%s\n", r0.c_str());
            m.sortByDisplayColumn(-1, Qt::AscendingOrder);   // reset
        }
        // Optional filter check: VVG_FILTER="<expr>".
        if (const char* fe = std::getenv("VVG_FILTER"); fe && *fe) {
            FilterExpr fx; std::string err;
            if (parse_filter_expr(fe, *m.source()->schema(), &fx, &err)) {
                m.setFilter(fx);
                std::printf("filter '%s' -> %lld/%lld rows\n", fe,
                            (long long)m.viewRows(), (long long)m.sourceTotal());
            } else {
                std::printf("filter parse error: %s\n", err.c_str());
            }
        }
        // Optional search check: VVG_FIND="<regex>".
        if (const char* se = std::getenv("VVG_FIND"); se && *se) {
            m.setSearch(QRegularExpression(se, QRegularExpression::CaseInsensitiveOption));
            QModelIndex hit = m.findNext(QModelIndex(), true);
            if (hit.isValid())
                std::printf("find '%s' -> row %d col %d = %s\n", se,
                            hit.row(), hit.column(),
                            m.data(hit, Qt::DisplayRole).toString().toStdString().c_str());
            else
                std::printf("find '%s' -> no match\n", se);
        }
        // Optional stats check: VVG_STATS=<displayColIndex>.
        if (const char* sc = std::getenv("VVG_STATS"); sc && *sc) {
            ColStats s = m.columnStats(std::atoi(sc));
            if (s.valid) {
                std::printf("stats[%s] %s count=%lld nulls=%lld", sc,
                            s.type.c_str(), (long long)s.count, (long long)s.nulls);
                if (s.is_numeric && s.count > 0)
                    std::printf(" min=%g max=%g mean=%g", s.min, s.max, s.mean);
                else if (s.count > 0)
                    std::printf(" smin=%s smax=%s", s.s_min.c_str(), s.s_max.c_str());
                std::printf(" distinct=%s\n",
                    s.distinct_overflow ? ">16"
                                        : std::to_string(s.distinct.size()).c_str());
            }
        }
        return 0;
    }

    MainWindow win(std::move(sources));
    win.setWindowTitle(QString::fromStdString(path) + " — vv");
    win.show();
    return app.exec();
}
