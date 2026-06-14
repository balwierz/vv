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
#include <QComboBox>
#include <QDockWidget>
#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QToolBar>
#include <QUrl>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "arrowtablemodel.h"
#include "vv/vvcore.hpp"

// Open every path through libvvcore (using a shared base Config so a later
// region/pileup query applies uniformly) and flatten each file's sibling tabs
// — sheets / tables / datasets / NPZ arrays — into one source list. Per-path
// open errors are collected rather than thrown; `opened` lists the paths that
// produced at least one source (for the recent-files list).
struct LoadResult {
    std::vector<std::unique_ptr<TabularSource>> sources;
    QStringList                                 opened;
    QStringList                                 errors;
};
static LoadResult loadSources(const Config& base, const QStringList& paths) {
    LoadResult r;
    for (const QString& p : paths) {
        Config cfg = base;
        cfg.path = p.toStdString();
        std::unique_ptr<TabularSource> src;
        std::string err = open_source(cfg.path, cfg, &src);
        if (!err.empty() || !src) {
            r.errors << QStringLiteral("%1: %2").arg(
                p, QString::fromStdString(err.empty() ? "open failed" : err));
            continue;
        }
        auto siblings = src->expand_tabs();
        r.sources.push_back(std::move(src));
        for (auto& s : siblings) r.sources.push_back(std::move(s));
        r.opened << p;
    }
    return r;
}

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(std::vector<std::unique_ptr<TabularSource>> sources = {}) {
        tabs_ = new QTabWidget(this);
        tabs_->setTabsClosable(true);
        tabs_->setDocumentMode(true);
        setCentralWidget(tabs_);
        setAcceptDrops(true);

        connect(tabs_, &QTabWidget::tabCloseRequested, this,
                [this](int i){ closeTab(i); });
        connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
            refreshStatus();
            refreshColumnsMenu();
            if (auto* v = activeView())
                updateDetail(v->selectionModel()->currentIndex());
        });

        buildMenus();
        buildDetailDock();
        buildToolbar();
        buildSearchBar();
        buildRegionBar();
        new QShortcut(QKeySequence::Copy, this, [this]{ copySelection(); });
        new QShortcut(QKeySequence::FindNext, this, [this]{ doFind(true); });

        for (auto& src : sources) addSourceTab(std::move(src));

        resize(1100, 760);
        refreshStatus();
    }

    // Open each path through libvvcore (with the current session region/coords/
    // slop/pileup) and add the resulting flattened tabs. Public so the headless
    // window self-test (VVG_WINTEST) can drive it.
    void openPaths(const QStringList& paths, bool quiet = false) {
        if (paths.isEmpty()) return;
        lastDir_ = QFileInfo(paths.first()).absolutePath();
        LoadResult r = loadWithSession(paths);
        for (auto& s : r.sources) addSourceTab(std::move(s));
        for (const QString& p : r.opened) { addRecent(p); openedPaths_ << p; }
        reportLoadErrors(r, quiet, tr("vvg — open"));
        if (!r.opened.isEmpty())
            setWindowTitle(r.opened.last() + QStringLiteral(" — vv"));
        refreshStatus();
    }
    int tabCount() const { return tabs_->count(); }
    int firstTabRows() const { return models_.empty() ? 0 : models_.front()->rowCount(); }
    // Headless mode (window self-test): route would-be modal dialogs to stderr
    // so an offscreen run never blocks on a QMessageBox::exec().
    void setHeadless(bool h) { headless_ = h; }
    // Programmatic region query for the self-test / future scripting.
    void applyRegionQuery(const QString& region, bool ncbi = false,
                          int slop = 0, bool pileup = false) {
        if (regionEdit_)  regionEdit_->setText(region);
        if (coordsCombo_) coordsCombo_->setCurrentIndex(ncbi ? 1 : 0);
        if (slopSpin_)    slopSpin_->setValue(slop);
        if (pileupAction_) pileupAction_->setChecked(pileup);
        applyRegion();
    }

private:
    static std::string src_label(ArrowTableModel* m) {
        return m->source()->tab_label();
    }

    // Build one model+view tab from a source and wire sort + detail-pane.
    void addSourceTab(std::unique_ptr<TabularSource> src) {
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
        int idx = tabs_->addTab(view, QString::fromStdString(src_label(model)));
        tabs_->setCurrentIndex(idx);
    }

    void closeTab(int i) {
        if (i < 0 || i >= tabs_->count()) return;
        // views_/models_ are kept index-aligned with the tab order.
        ArrowTableModel* m = (i < (int)models_.size()) ? models_[i] : nullptr;
        QWidget* w = tabs_->widget(i);
        tabs_->removeTab(i);
        if (i < (int)views_.size()) {
            views_.erase(views_.begin() + i);
            models_.erase(models_.begin() + i);
        }
        if (m) sortOrder_.remove(m);
        delete w;   // QTableView (detaches from its model)
        delete m;   // model is parented to MainWindow; free it now
        refreshStatus();
    }

    void buildMenus() {
        auto* file = menuBar()->addMenu(tr("&File"));
        QAction* open = new QAction(tr("&Open…"), this);
        open->setShortcut(QKeySequence::Open);
        connect(open, &QAction::triggered, this, [this]{ openFilesDialog(); });
        file->addAction(open);
        recentMenu_ = file->addMenu(tr("Open &Recent"));
        rebuildRecentMenu();
        file->addSeparator();
        QAction* close = new QAction(tr("&Close Tab"), this);
        close->setShortcut(QKeySequence::Close);
        connect(close, &QAction::triggered, this, [this]{ closeTab(tabs_->currentIndex()); });
        file->addAction(close);
        file->addSeparator();
        QAction* quit = new QAction(tr("&Quit"), this);
        quit->setShortcut(QKeySequence::Quit);
        connect(quit, &QAction::triggered, qApp, &QApplication::quit);
        file->addAction(quit);

        auto* edit = menuBar()->addMenu(tr("&Edit"));
        QAction* cp = new QAction(tr("&Copy"), this);
        cp->setShortcut(QKeySequence::Copy);
        connect(cp, &QAction::triggered, this, [this]{ copySelection(); });
        edit->addAction(cp);

        auto* view = menuBar()->addMenu(tr("&View"));
        QAction* go = new QAction(tr("&Go to Row…"), this);
        go->setShortcut(QKeySequence(QStringLiteral("Ctrl+G")));
        connect(go, &QAction::triggered, this, [this]{ gotoRow(); });
        view->addAction(go);
        view->addSeparator();
        columnsMenu_ = view->addMenu(tr("&Columns"));
        refreshColumnsMenu();

        auto* help = menuBar()->addMenu(tr("&Help"));
        connect(help->addAction(tr("&Shortcuts && Syntax")), &QAction::triggered, this, [this]{
            QMessageBox::information(this, tr("vvg — help"),
                tr("Sort:    click a column header (toggles ascending/descending)\n"
                   "Filter:  the Filter bar — e.g.  score > 5 and chrom == \"chr1\"\n"
                   "Find:    the Find bar (regex); Ctrl+F / F3 for next match\n"
                   "Region:  the Region bar — chr1:1000-2000 (UCSC) / NCBI; Pileup for BAM\n"
                   "Copy:    Ctrl+C copies the selection as TSV\n"
                   "Slice:   ◀ / ▶ steps the leading axis of NPZ 3-D+ arrays\n"
                   "Go to:   Ctrl+G jumps to a row; View ▸ Columns shows/hides columns"));
        });
        connect(help->addAction(tr("&About vvg")), &QAction::triggered, this, [this]{
            QMessageBox::about(this, tr("vvg"),
                tr("vv — graphical viewer.\n\nOpens every format the vv CLI supports "
                   "(Parquet, Arrow, BAM/VCF/BCF, BED/GFF, HDF5/AnnData, NPZ, xlsx/ods, "
                   "SQLite, …) through the shared libvvcore reader core."));
        });
    }

    // Rebuild the View ▸ Columns checkable list for the active table.
    void refreshColumnsMenu() {
        if (!columnsMenu_) return;
        columnsMenu_->clear();
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v || m->displayColumnCount() == 0) {
            columnsMenu_->addAction(tr("(no columns)"))->setEnabled(false);
            return;
        }
        for (int c = 0; c < m->displayColumnCount(); ++c) {
            QAction* a = columnsMenu_->addAction(m->columnName(c));
            a->setCheckable(true);
            a->setChecked(!v->isColumnHidden(c));
            connect(a, &QAction::toggled, this, [v, c](bool on){
                v->setColumnHidden(c, !on);
            });
        }
    }

    void gotoRow() {
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v || m->rowCount() == 0) return;
        bool ok = false;
        int row = QInputDialog::getInt(this, tr("Go to row"), tr("Row (1-based):"),
                                       1, 1, m->rowCount(), 1, &ok);
        if (ok) {
            QModelIndex idx = m->index(row - 1, 0);
            v->setCurrentIndex(idx);
            v->scrollTo(idx, QAbstractItemView::PositionAtTop);
        }
    }

    void openFilesDialog() {
        QStringList files = QFileDialog::getOpenFileNames(
            this, tr("Open data file(s)"), lastDir_, tr("All files (*)"));
        openPaths(files);
    }

    void addRecent(const QString& path) {
        QSettings s(QStringLiteral("vv"), QStringLiteral("vvg"));
        QStringList recent = s.value(QStringLiteral("recentFiles")).toStringList();
        QString abs = QFileInfo(path).absoluteFilePath();
        recent.removeAll(abs);
        recent.prepend(abs);
        while (recent.size() > 10) recent.removeLast();
        s.setValue(QStringLiteral("recentFiles"), recent);
        rebuildRecentMenu();
    }
    void rebuildRecentMenu() {
        if (!recentMenu_) return;
        recentMenu_->clear();
        QStringList recent = QSettings(QStringLiteral("vv"), QStringLiteral("vvg"))
                                 .value(QStringLiteral("recentFiles")).toStringList();
        if (recent.isEmpty()) {
            recentMenu_->addAction(tr("(none)"))->setEnabled(false);
            return;
        }
        for (const QString& p : recent)
            connect(recentMenu_->addAction(p), &QAction::triggered, this,
                    [this, p]{ openPaths({p}); });
        recentMenu_->addSeparator();
        connect(recentMenu_->addAction(tr("Clear Recent")), &QAction::triggered, this, [this]{
            QSettings(QStringLiteral("vv"), QStringLiteral("vvg"))
                .remove(QStringLiteral("recentFiles"));
            rebuildRecentMenu();
        });
    }

    // Open paths with the current session Config (region/coords/slop/pileup),
    // canonicalising the region first. Errors are collected, not thrown.
    LoadResult loadWithSession(const QStringList& paths) {
        Config base = sessionCfg_;
        std::string err = apply_region_modifiers(base);
        if (!err.empty()) {
            LoadResult r; r.errors << QString::fromStdString(err); return r;
        }
        return loadSources(base, paths);
    }
    void reportLoadErrors(const LoadResult& r, bool quiet, const QString& title) {
        if (r.errors.isEmpty()) return;
        QString joined = r.errors.join(QLatin1Char('\n'));
        if (quiet || headless_)
            std::fprintf(stderr, "vvg: %s\n", joined.toStdString().c_str());
        else
            QMessageBox::warning(this, title, joined);
    }

    void buildRegionBar() {
        auto* tb = addToolBar(tr("Region"));
        tb->addWidget(new QLabel(tr("  Region: ")));
        regionEdit_ = new QLineEdit(tb);
        regionEdit_->setPlaceholderText(tr("chr1:1000-2000[,chr2:…]"));
        regionEdit_->setClearButtonEnabled(true);
        regionEdit_->setMinimumWidth(200);
        tb->addWidget(regionEdit_);
        connect(regionEdit_, &QLineEdit::returnPressed, this, [this]{ applyRegion(); });

        coordsCombo_ = new QComboBox(tb);
        coordsCombo_->addItem(tr("UCSC 0-based"));   // index 0
        coordsCombo_->addItem(tr("NCBI 1-based"));   // index 1
        coordsCombo_->setToolTip(tr("Coordinate convention for the region box"));
        tb->addWidget(coordsCombo_);

        tb->addWidget(new QLabel(tr(" slop ")));
        slopSpin_ = new QSpinBox(tb);
        slopSpin_->setRange(0, 1000000000);
        slopSpin_->setSingleStep(100);
        slopSpin_->setToolTip(tr("Pad each window by N bp on both sides"));
        tb->addWidget(slopSpin_);

        connect(tb->addAction(tr("Apply")), &QAction::triggered, this,
                [this]{ applyRegion(); });
        connect(tb->addAction(tr("Clear")), &QAction::triggered, this,
                [this]{ regionEdit_->clear(); applyRegion(); });

        pileupAction_ = tb->addAction(tr("Pileup"));
        pileupAction_->setCheckable(true);
        pileupAction_->setToolTip(
            tr("BAM/CRAM: emit samtools mpileup-style per-base rows"));
        connect(pileupAction_, &QAction::toggled, this, [this](bool){ applyRegion(); });
    }

    void applyRegion() {
        sessionCfg_.region           = regionEdit_->text().trimmed().toStdString();
        sessionCfg_.coords_one_based = (coordsCombo_->currentIndex() == 1);
        sessionCfg_.slop             = slopSpin_->value();
        sessionCfg_.pileup           = pileupAction_->isChecked();
        reopenAll();
    }

    // Re-open every currently-loaded file under the session Config. Transactional:
    // load the new sources first and only replace the tab set if at least one
    // opened — a failed region query (e.g. un-indexed file) keeps the old view.
    void reopenAll() {
        if (openedPaths_.isEmpty()) return;
        LoadResult r = loadWithSession(openedPaths_);
        if (r.sources.empty()) {
            QString why = r.errors.isEmpty() ? tr("no data for this query")
                                             : r.errors.join(QLatin1Char('\n'));
            if (headless_)
                std::fprintf(stderr, "vvg region: %s\n", why.toStdString().c_str());
            else
                QMessageBox::warning(this, tr("vvg — region"),
                    tr("Nothing to show — keeping the current view.\n\n%1\n\n"
                       "(Tabix/BCF region queries need a .tbi/.csi index; Pileup "
                       "needs a BAM/CRAM file.)").arg(why));
            return;
        }
        while (tabs_->count() > 0) closeTab(0);
        for (auto& s : r.sources) addSourceTab(std::move(s));
        reportLoadErrors(r, /*quiet=*/false, tr("vvg — region"));
        refreshStatus();
    }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        QStringList paths;
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile()) paths << u.toLocalFile();
        openPaths(paths);
    }

private:
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
    QLineEdit*                     regionEdit_ = nullptr;
    QComboBox*                     coordsCombo_ = nullptr;
    QSpinBox*                      slopSpin_   = nullptr;
    QAction*                       pileupAction_ = nullptr;
    QMenu*                         recentMenu_  = nullptr;
    QMenu*                         columnsMenu_ = nullptr;
    QString                        lastDir_;
    bool                           headless_ = false;   // suppress modal dialogs
    QStringList                    openedPaths_;   // files currently loaded
    Config                         sessionCfg_;    // region/coords/slop/pileup
    std::vector<QTableView*>       views_;
    std::vector<ArrowTableModel*>  models_;
    QMap<ArrowTableModel*, Qt::SortOrder> sortOrder_;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QStringList paths;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (!a.empty() && a[0] != '-') paths << QString::fromStdString(a);
    }

    if (const char* st = std::getenv("VVG_SELFTEST"); st && *st && *st != '0') {
        // Model-level self-test (CI path): needs a file on the command line.
        if (paths.isEmpty()) { std::fprintf(stderr, "usage: vvg <file>\n"); return 2; }
        Config cfg;
        cfg.path = paths.first().toStdString();
        std::unique_ptr<TabularSource> src;
        std::string err = open_source(cfg.path, cfg, &src);
        if (!err.empty()) { std::fprintf(stderr, "vvg: %s\n", err.c_str()); return 1; }
        std::vector<std::unique_ptr<TabularSource>> sources;
        auto siblings = src->expand_tabs();
        sources.push_back(std::move(src));
        for (auto& s : siblings) sources.push_back(std::move(s));

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

    // Window-level offscreen smoke test: build the shell and open the file(s)
    // headlessly (quiet = no blocking modal dialogs). Exercises the
    // addSourceTab / loadSources path the menus and drag-and-drop also use.
    if (const char* wt = std::getenv("VVG_WINTEST"); wt && *wt && *wt != '0') {
        MainWindow win;
        win.setHeadless(true);
        win.openPaths(paths, /*quiet=*/true);
        std::printf("win_tabs=%d\n", win.tabCount());
        // Optional region/pileup re-open check: VVG_REGION="chr1:…" [VVG_NCBI=1]
        // [VVG_PILEUP=1].
        const char* rg = std::getenv("VVG_REGION");
        bool pileup = std::getenv("VVG_PILEUP") != nullptr;
        if ((rg && *rg) || pileup) {
            win.applyRegionQuery(QString::fromLocal8Bit(rg ? rg : ""),
                                 std::getenv("VVG_NCBI") != nullptr, 0, pileup);
            std::printf("region '%s' pileup=%d -> tabs=%d rows=%d\n",
                        rg ? rg : "", pileup ? 1 : 0,
                        win.tabCount(), win.firstTabRows());
        }
        return 0;
    }

    MainWindow win;
    win.show();
    if (!paths.isEmpty()) win.openPaths(paths);
    else                  win.setWindowTitle(QStringLiteral("vv"));
    return app.exec();
}
