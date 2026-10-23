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

// Arrow's I/O headers before Qt's: Qt's `signals` macro breaks arrow/util/cancel.h.
#include <arrow/io/compressed.h>
#include <arrow/io/file.h>
#include <arrow/io/memory.h>
#include <arrow/util/compression.h>

#include <QApplication>
#include <QStandardPaths>
#include <QProcess>
#include <QFontDatabase>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QStringListModel>
#include <QCompleter>
#include <QIcon>
#include <QClipboard>
#include <QComboBox>
#include <QDockWidget>
#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGraphicsOpacityEffect>
#include <QHash>
#include <QPainter>
#include <QPropertyAnimation>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>
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
#include <QMouseEvent>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryFile>
#include <QToolBar>
#include <QToolButton>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QPolygonF>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <map>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "arrowtablemodel.h"
#include "jsontreemodel.h"
#include "vvcommand.h"
#include "columnpanel.h"
#include "contextbar.h"
#include "fileoptions.h"
#include "tabpane.h"
#include "typedheader.h"
#include "tableview.h"
#include "loadsources.h"
#include "columnwidths.h"
#include "cursordelegate.h"
#include "sequencedelegate.h"
#include "viewstate.h"
#include "helpdialog.h"
#include "shortcutsdialog.h"
#include "actionpalette.h"
#include "vv/vvcore.hpp"


// A highlight over `r` of `host` that fades out, drawing the eye to a column
// just scrolled into view.
static void flashRect(QWidget* host, const QRect& r) {
    auto* w = new QWidget(host);
    w->setAttribute(Qt::WA_TransparentForMouseEvents);
    w->setAutoFillBackground(true);
    QPalette pal = w->palette();
    QColor c = host->palette().color(QPalette::Highlight);
    c.setAlpha(110);
    pal.setColor(QPalette::Window, c);
    w->setPalette(pal);
    w->setGeometry(r);
    auto* eff = new QGraphicsOpacityEffect(w);
    w->setGraphicsEffect(eff);
    auto* anim = new QPropertyAnimation(eff, "opacity", w);
    anim->setDuration(900);
    anim->setStartValue(1.0);
    anim->setEndValue(0.0);
    anim->setEasingCurve(QEasingCurve::InQuad);
    QObject::connect(anim, &QPropertyAnimation::finished, w, &QObject::deleteLater);
    w->show();
    anim->start();
}

// Toolbar icons where the icon theme has none: monochrome strokes in the
// window text colour, drawn on a 16-unit grid with gg's line weight (its
// bundled SVGs), so vvg needs no SVG module.
enum class Glyph { Menu, Details, Export, Filter, Find, Open, Directory, Copy, CopyRow, History };
static QIcon glyphIcon(Glyph g, const QColor& color) {
    QIcon icon;
    for (int px : {16, 20, 22, 24, 32, 48}) {
        QPixmap pm(px, px);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(px / 16.0, px / 16.0);
        p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        switch (g) {
        case Glyph::Menu:                     // three bars
            for (double y : {4.0, 8.0, 12.0}) p.drawLine(QPointF(2.5, y), QPointF(13.5, y));
            break;
        case Glyph::Details:                  // a window with a panel on the right
            p.drawRoundedRect(QRectF(1.75, 2.75, 12.5, 10.5), 1.5, 1.5);
            p.drawLine(QPointF(10, 2.75), QPointF(10, 13.25));
            p.drawLine(QPointF(11.3, 5.5), QPointF(12.7, 5.5));
            p.drawLine(QPointF(11.3, 8), QPointF(12.7, 8));
            break;
        case Glyph::Export:                   // an arrow out of a tray
            p.drawPolyline(QPolygonF({QPointF(2.75, 9.5), QPointF(2.75, 13.25),
                                      QPointF(13.25, 13.25), QPointF(13.25, 9.5)}));
            p.drawLine(QPointF(8, 10.5), QPointF(8, 2.75));
            p.drawPolyline(QPolygonF({QPointF(5, 5.75), QPointF(8, 2.75), QPointF(11, 5.75)}));
            break;
        case Glyph::Filter:                   // a funnel
            p.drawPolygon(QPolygonF({QPointF(2.25, 3), QPointF(13.75, 3), QPointF(9.5, 8.25),
                                     QPointF(9.5, 13), QPointF(6.5, 11.5), QPointF(6.5, 8.25)}));
            break;
        case Glyph::Find:                     // a magnifier
            p.drawEllipse(QPointF(6.75, 6.75), 4.25, 4.25);
            p.drawLine(QPointF(9.9, 9.9), QPointF(13.5, 13.5));
            break;
        case Glyph::History:                  // a clock face
            p.drawEllipse(QPointF(8, 8), 5.75, 5.75);
            p.drawPolyline(QPolygonF({QPointF(8, 4.5), QPointF(8, 8), QPointF(10.5, 9.5)}));
            break;
        case Glyph::Open:                     // a sheet with a folded corner
            p.drawPolygon(QPolygonF({QPointF(3.25, 1.75), QPointF(9.5, 1.75), QPointF(12.75, 5),
                                     QPointF(12.75, 14.25), QPointF(3.25, 14.25)}));
            p.drawPolyline(QPolygonF({QPointF(9.5, 1.75), QPointF(9.5, 5), QPointF(12.75, 5)}));
            break;
        case Glyph::Directory:                // a folder
            p.drawPolygon(QPolygonF({QPointF(1.75, 3.25), QPointF(6, 3.25), QPointF(7.5, 4.75),
                                     QPointF(14.25, 4.75), QPointF(14.25, 12.75), QPointF(1.75, 12.75)}));
            p.drawLine(QPointF(1.75, 6.75), QPointF(14.25, 6.75));
            break;
        case Glyph::Copy:                     // two overlapping sheets
        case Glyph::CopyRow:                  // … the front one with a row marked
            p.drawRoundedRect(QRectF(1.75, 1.75, 8.5, 9.5), 1, 1);
            p.setBrush(QColor(0, 0, 0, 0));
            p.drawRoundedRect(QRectF(5.75, 4.75, 8.5, 9.5), 1, 1);
            if (g == Glyph::CopyRow) {
                p.setBrush(color);
                p.drawRect(QRectF(5.75, 8, 8.5, 2.5));
            }
            break;
        }
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

static_assert(TypedHeaderView::TypeRole == ArrowTableModel::TypeRole);

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(std::vector<std::unique_ptr<TabularSource>> sources = {}) {
        tabs_ = new TabPane(this);
        contextBar_ = new ContextBar;
        tabs_->setStrip(contextBar_);
        connect(contextBar_, &ContextBar::optionsChanged, this, [this](const FileOptions& o) {
            if (const OpenFile* f = activeFile()) reopenFile(f->id, o);
        });
        connect(contextBar_, &ContextBar::sliceStep, this, [this](int d) { stepSlice(d); });
        connect(contextBar_, &ContextBar::reloadRequested, this, [this] {
            if (const OpenFile* f = activeFile()) reloadFile(f->id);
        });
        connect(contextBar_, &ContextBar::ignoreChangeRequested, this, [this] {
            if (const OpenFile* f = activeFile()) ignoreChange(f->id);
        });
        connect(contextBar_, &ContextBar::loadMoreRequested, this, [this] { loadMoreRows(); });
        tabs_->setTabsClosable(true);
        tabs_->setDocumentMode(true);
        setCentralWidget(tabs_);
        setAcceptDrops(true);

        connect(tabs_, &TabPane::tabCloseRequested, this,
                [this](int i){ closeTab(i); });
        connect(tabs_, &TabPane::currentChanged, this, [this](int i) { onCurrentTab(i); });
        // Tabs of one file share a colour: a line under them in the tab bar
        // (with two files or more) and a square by the file's name in the strip.
        tabs_->setGroups([this](int i) { return tabFile_.value(tabs_->widget(i), 0); },
                         [this](int id) { return fileColor(id); });
        tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tabs_->tabBar(), &QWidget::customContextMenuRequested, this,
                [this](const QPoint& p) { tabMenu(p); });

        // Settings are read here, once; writes go to a thread of their own
        // (saveSetting), because QSettings syncs the file with fdatasync()
        // and a rename, which took from 15 ms to seconds on a busy disk.
        settingsThread_ = new QThread(this);
        settingsCtx_ = new QObject;
        settingsCtx_->moveToThread(settingsThread_);
        connect(settingsThread_, &QThread::finished, settingsCtx_, &QObject::deleteLater);
        settingsThread_->start();
        {
            QSettings st(QStringLiteral("vv"), QStringLiteral("vvg"));
            showTypes_ = st.value(QStringLiteral("ui/showColumnTypes"), true).toBool();
            smoothScroll_ = st.value(QStringLiteral("smoothScroll"), true).toBool();
            recent_ = st.value(QStringLiteral("recentFiles")).toStringList();
            openFilter_ = st.value(QStringLiteral("ui/openFilter")).toString();
            restoreSession_ = st.value(QStringLiteral("ui/restoreSession"), true).toBool();
            loadBudget_ = st.value(QStringLiteral("limits/loadBytes"),
                                   (qlonglong)ArrowTableModel::kDefaultLoadBudget).toLongLong();
            if (const char* e = std::getenv("VVG_STREAM_BUDGET"); e && *e)   // self-test
                loadBudget_ = std::atoll(e);
            passRows_ = st.value(QStringLiteral("limits/passRows"),
                                 (qlonglong)ArrowTableModel::kDefaultPassRows).toLongLong();
            passBytes_ = st.value(QStringLiteral("limits/passBytes"),
                                  (qlonglong)ArrowTableModel::kDefaultPassBytes).toLongLong();
            statsAutoRows_ = passRows_;
            for (const char* kind : {"recent", "saved"}) {
                QSettings& rs = st;
                rs.beginGroup(QStringLiteral("filters/%1").arg(QLatin1String(kind)));
                for (const QString& k : rs.childKeys()) {
                    if (!std::strcmp(kind, "recent")) recentFilters_[k] = rs.value(k).toStringList();
                    else savedFilters_[k] = rs.value(k).toMap();
                }
                rs.endGroup();
            }
        }
        buildDetailDock();
        buildMenus();
        buildToolbar();
        buildProgressUI();

        for (auto& src : sources) {
            QString origin = QString::fromStdString(src->path());
            addSourceTab(std::move(src), origin, true);
        }

        resize(1100, 760);
        // The window's size and its dock / toolbar layout from the last run.
        const QSettings st(QStringLiteral("vv"), QStringLiteral("vvg"));
        restoreGeometry(st.value(QStringLiteral("geometry")).toByteArray());
        restoreState(st.value(QStringLiteral("windowState")).toByteArray());
        compactToolbar(true);
        refreshStatus();
    }

    // Open each path through libvvcore (with the current session region/coords/
    // slop/pileup) and add the resulting flattened tabs. Public so the headless
    // window self-test (VVG_WINTEST) can drive it.
    void openPaths(const QStringList& paths_in, bool quiet = false) {
        if (paths_in.isEmpty()) return;
        if (!is_remote_url(paths_in.first().toStdString()))
            lastDir_ = QFileInfo(paths_in.first()).absolutePath();
        // Show the first file's first tab once all are open (the first tab
        // added when that file failed to open).
        const int firstNew = tabs_->count();
        int firstFileTab = -1;
        // JSON documents open as a tree (as in vv on a terminal); JSON Lines,
        // and any file when vv's table flags were given, as a table.
        QStringList errors, opened;
        for (const QString& p : paths_in) {
            if (!jsonAsTable_ && jsonDocumentPath(p)) {
                const int id = nextFileId_++;
                const QString e = openJsonTreeTab(p, id);
                if (e.isEmpty()) {
                    if (p == paths_in.first()) firstFileTab = tabs_->count() - 1;
                    files_.push_back(OpenFile{id, p, newFileOptions_, /*jsonTree=*/true});
                    addRecent(p);
                    continue;
                }
                if (headless_ || quiet) std::fprintf(stderr, "vvg: %s\n", e.toLocal8Bit().constData());
                else QMessageBox::warning(this, tr("vvg — open"), e);
                continue;
            }
            OpenFile f{nextFileId_++, p, newFileOptions_, false};
            f.feat = file_features(p.toStdString());
            LoadResult r = loadFile(f.path, f.opts);
            errors << r.errors;
            if (r.sources.empty()) continue;
            files_.push_back(f);
            for (size_t i = 0; i < r.sources.size(); ++i) {
                if (firstFileTab < 0 && r.primary[i] && p == paths_in.first())
                    firstFileTab = tabs_->count();
                addSourceTab(std::move(r.sources[i]), r.origins[i], r.primary[i], r.expands[i],
                             r.owners[i], f.id);
            }
            addRecent(p);
            opened << p;
        }
        if (firstFileTab < 0 && tabs_->count() > firstNew) firstFileTab = firstNew;
        if (firstFileTab >= 0) tabs_->setCurrentIndex(firstFileTab);
        if (!errors.isEmpty()) {
            const QString joined = errors.join(QLatin1Char('\n'));
            if (quiet || headless_) std::fprintf(stderr, "vvg: %s\n", joined.toStdString().c_str());
            else QMessageBox::warning(this, tr("vvg — open"), joined);
        }
        for (const OpenFile& f : files_) if (!stamps_.contains(f.id)) stampFile(f.id);
        relabelTabs();
        updateTitle();
        refreshStatus();
    }
    int tabCount() const { return tabs_->count(); }
    int firstTabRows() const { return models_.empty() || !models_.front() ? 0 : models_.front()->rowCount(); }
    int firstTabCols() const { return models_.empty() || !models_.front() ? 0 : models_.front()->columnCount(); }
    // Open JSON documents as tables instead of trees (vv's table flags given).
    void setJsonAsTable(bool on) { jsonAsTable_ = on; }
    // Self-test: Find in the JSON tree tab, wait for the scan, print the
    // selected path and the number of highlighted rows on screen.
    void jsonFindForTest(const QString& q, bool forward) {
        JsonTab* jt = activeJsonTab();
        if (!jt && !jsonTabs_.empty()) { tabs_->setCurrentWidget(jsonTabs_.begin()->first); jt = activeJsonTab(); }
        if (!jt) { std::printf("json-find: no tree tab\n"); return; }
        findEdit_->setText(q);
        doJsonFind(jt, forward);
        while (jt->searching()) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::processEvents();
        const QModelIndex cur = jt->view->currentIndex();
        std::printf("json-find '%s' %s -> %s%s\n", q.toLocal8Bit().constData(), forward ? "fwd" : "back",
                    cur.isValid() ? jt->model->path(cur).toLocal8Bit().constData() : "(none)",
                    statusBar()->currentMessage().contains(QStringLiteral("no match")) ? " (no match)"
                    : statusBar()->currentMessage().contains(QStringLiteral("wrapped")) ? " (wrapped)" : "");
    }
    // Self-test: in the active JSON tree tab, select `path` (expanding to it)
    // and print it; with `table`, open the records there as a table tab.
    void jsonTreeForTest(const QString& path, bool table) {
        JsonTab* jt = activeJsonTab();
        if (!jt && !jsonTabs_.empty()) {
            tabs_->setCurrentWidget(jsonTabs_.begin()->first);
            jt = activeJsonTab();
        }
        if (!jt) { std::printf("json-tree: no tree tab\n"); return; }
        QModelIndex at = jt->model->index(0, 0);
        jt->view->expand(at);
        if (!path.isEmpty() && path != QStringLiteral(".")) {
            at = findJsonPath(jt, path);
            if (!at.isValid()) { std::printf("json-tree: no %s\n", path.toLocal8Bit().constData()); return; }
        }
        jt->view->setCurrentIndex(at);
        std::printf("json-tree path=%s type=%s children=%d value=%s\n",
                    jt->model->path(at).toLocal8Bit().constData(),
                    jt->model->index(at.row(), JsonTreeModel::Type, at.parent()).data().toString().toLocal8Bit().constData(),
                    jt->model->rowCount(at),
                    jt->model->index(at.row(), JsonTreeModel::Value, at.parent()).data().toString().left(60).toLocal8Bit().constData());
        if (table) {
            openRecordsTable(jt, at);
            waitIdleForTest();
            std::printf("json-tree table: tabs=%d rows=%d cols=%d label=%s\n", tabs_->count(),
                        activeModel() ? activeModel()->rowCount() : -1,
                        activeModel() ? activeModel()->columnCount() : -1,
                        activeModel() ? activeModel()->tabLabel().c_str() : "");
        }
    }
    // Print every open tab's materialised dimensions — used by the self-test to
    // confirm dense 2-D matrices are previewed (row/col capped) rather than
    // fully densified.
    void dumpTabDims() {
        materializeAllTabs();
        waitIdleForTest();
        for (auto* m : models_)
            if (m) std::printf("tab '%s' rows=%d cols=%d footer=%s\n",
                        m->tabLabel().c_str(),
                        m->rowCount(), m->displayColumnCount(),
                        m->footer().toStdString().c_str());
    }
    // Drive the *async* filter end-to-end for the self-test: launch it, pump the
    // event loop until the worker's result is installed, return the row count.
    int applyFilterAsyncForTest(const QString& expr) {
        auto* m = activeModel();
        if (!m) return -1;
        FilterExpr fx; std::string e;
        if (!parse_filter_expr(expr.toStdString(), *m->schema(), &fx, &e))
            return -2;
        filterText_[m] = expr;
        m->setFilterAsync(fx);
        for (int guard = 0; m->isComputing() && guard < 2000000; ++guard)
            QCoreApplication::processEvents();
        return (int)m->viewRows();
    }
    // Window self-test of superseded jobs: a filter, then a sort started
    // before the filter finished (the filter job is cancelled; the sort queues
    // behind it on the source's owner thread, filtered or not as the model
    // holds it). Prints the rows and the first cell. Then a filter, a sort and
    // a find started back to back, and the tab closed while they run: the
    // jobs stop at their next chunk and nothing is delivered to the closed tab.
    QString raceForTest(const QString& expr) {
        auto* m = activeModel();
        if (!m) return QStringLiteral("no model");
        FilterExpr fx; std::string e;
        if (!parse_filter_expr(expr.toStdString(), *m->schema(), &fx, &e))
            return QStringLiteral("bad filter: ") + QString::fromStdString(e);
        m->setFilterAsync(fx);
        m->sortAsyncByDisplayColumn(0, Qt::DescendingOrder);
        for (int guard = 0; m->isComputing() && guard < 2000000; ++guard)
            QCoreApplication::processEvents();
        QString out;
        {
            ArrowTableModel::SyncReads now(m);
            out = QStringLiteral("rows=%1 first=%2")
                .arg(m->viewRows()).arg(m->data(m->index(0, 0), Qt::DisplayRole).toString());
        }
        m->setFilterAsync(fx);
        m->sortAsyncByDisplayColumn(0, Qt::AscendingOrder);
        m->findAsync(QRegularExpression(QStringLiteral(".")));
        closeTab(tabs_->currentIndex());
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 300) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return out + QStringLiteral(" closed tabs=%1").arg(tabs_->count());
    }
    // Window self-test: put the cursor on (row, col) and report the detail
    // dock (row shown, its first two values, whether the value items were
    // reused).
    QString detailForTest(int row, int col) {
        auto* v = activeView();
        if (!v || !detail_) return QStringLiteral("no view");
        QTableWidgetItem* before = detail_->item(0, 1);
        v->selectionModel()->setCurrentIndex(v->model()->index(row, col),
                                             QItemSelectionModel::ClearAndSelect);
        waitIdleForTest();            // the row's chunk arrives, the dock refills
        QTableWidgetItem* first = detail_->item(0, 1);
        QStringList vals;
        for (int r = 0; r < std::min(2, detail_->rowCount()); ++r)
            vals << (detail_->item(r, 1) ? detail_->item(r, 1)->text() : QString());
        return QStringLiteral("row=%1 rows=%2 values=%3 reused=%4")
            .arg(detailRow_).arg(detail_->rowCount())
            .arg(vals.join(QLatin1Char(',')))
            .arg(before && before == first ? 1 : 0);
    }
    // Window self-test: put the cursor in column `col`, show its Column-tab
    // summary (scanning on the worker, or Compute for a tab that cannot be
    // re-read) and return what the panel shows.
    QString columnStatsForTest(int col) {
        waitIdleForTest();
        auto* v = activeView();
        if (!v) return QStringLiteral("no view");
        showColumnTab();
        v->selectionModel()->setCurrentIndex(v->model()->index(0, col),
                                             QItemSelectionModel::ClearAndSelect);
        return waitColumnStatsForTest();
    }
    // As columnStatsForTest, then a click on the Compute button when the
    // Column tab offers it.
    QString computeColumnStatsForTest(int col) {
        const QString first = columnStatsForTest(col);
        if (!first.endsWith(QLatin1Char(']'))) return first;
        computeColumnStatsRequested();
        return waitColumnStatsForTest();
    }
    void setStatsAutoRowsForTest(int64_t n) { statsAutoRows_ = n; }
    QString waitColumnStatsForTest() {
        refreshColumnStats(true);
        QElapsedTimer t;
        t.start();
        while (statsProg_ && t.elapsed() < 60000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(5);
        }
        auto* m = activeModel();
        if (m && !tabOrigin_.value(m).reopen && !colPanel_->summary()) {
            computeColumnStatsHere();
            return QStringLiteral("(here) ") + colPanel_->describeForTest();
        }
        return colPanel_->describeForTest();
    }
    // Fixed geometry for the location checks, independent of fonts:
    // 100-px columns in a 200-px view.
    void fixGeometryForTest(QTableView* v) {
        for (int c = 0; c < v->model()->columnCount(); ++c)
            v->horizontalHeader()->resizeSection(c, 100);
        v->resize(200, 300);
        // A view never shown lays out its header only when asked: its columns
        // arrived (async model) after it was created with none, and a hidden
        // widget gets no resize event, so the header's viewport stays empty.
        QMetaObject::invokeMethod(v, "updateGeometries", Qt::DirectConnection);
        QResizeEvent re(v->horizontalHeader()->size(), QSize());
        QCoreApplication::sendEvent(v->horizontalHeader(), &re);
        QCoreApplication::processEvents();
    }
    // Window self-test: cursor in column 0, then a mouse click on the header
    // of column `col` (scrolled into view first, as a user sees it). Prints
    // the sort column, the cursor column after the sort and the Column tab.
    QString headerClickForTest(int col) {
        waitIdleForTest();
        auto* v = activeView();
        auto* m = activeModel();
        if (!v || !m || col < 0 || col >= m->displayColumnCount()) return QStringLiteral("no column");
        showColumnTab();
        fixGeometryForTest(v);
        v->selectionModel()->setCurrentIndex(m->index(0, 0), QItemSelectionModel::ClearAndSelect);
        v->scrollTo(m->index(0, col));
        QHeaderView* h = v->horizontalHeader();
        const QPointF at(h->sectionViewportPosition(col) + h->sectionSize(col) / 2.0, h->height() / 2.0);
        for (QEvent::Type t : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent ev(t, at, h->viewport()->mapToGlobal(at), Qt::LeftButton,
                           t == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(h->viewport(), &ev);
        }
        for (int g = 0; m->isComputing() && g < 2000000; ++g) QCoreApplication::processEvents();
        const QModelIndex cur = v->currentIndex();
        const QString stats = waitColumnStatsForTest();   // before the title: it sets it
        return QStringLiteral("sort=%1 cursor=%2,%3 title=%4 where=%5 %6")
            .arg(m->sortColumn()).arg(cur.row()).arg(cur.column())
            .arg(colPanel_->titleForTest(), colPanel_->locationForTest(), stats);
    }
    // Window self-test: cursor in column `col`, the view scrolled back to
    // its left edge, then the Column tab's "show in table". Prints the
    // location before and after, and the column's position in the view.
    QString showColumnForTest(int col) {
        waitIdleForTest();
        auto* v = activeView();
        auto* m = activeModel();
        if (!v || !m || col < 0 || col >= m->displayColumnCount()) return QStringLiteral("no column");
        fixGeometryForTest(v);
        v->selectionModel()->setCurrentIndex(m->index(0, col), QItemSelectionModel::ClearAndSelect);
        v->horizontalScrollBar()->setValue(0);
        updateColumnLocation();
        const QString before = colPanel_->locationForTest();
        emit colPanel_->showColumnRequested();
        const int x = v->columnViewportPosition(col);
        return QStringLiteral("before=%1 after=%2 x=%3 visible=%4")
            .arg(before, colPanel_->locationForTest()).arg(x)
            .arg(x >= 0 && x + v->columnWidth(col) <= v->viewport()->width() ? 1 : 0);
    }
    // Window self-test: select the block (r0, c0)-(r1, c1) and return the
    // status bar's selection summary.
    QString selectionSummaryForTest(int r0, int c0, int r1, int c1) {
        waitIdleForTest();
        auto* v = activeView();
        if (!v) return QStringLiteral("no view");
        QItemSelection sel(v->model()->index(r0, c0), v->model()->index(r1, c1));
        v->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
        updateSelectionSummary();
        return selSum_->isVisible() || !selSum_->text().isEmpty() ? selSum_->text()
                                                                  : QStringLiteral("(none)");
    }
    // Drive the *async* find end-to-end: scan off-thread, pump until the match
    // list is installed, return the number of matches.
    qint64 findAsyncForTest(const QString& pattern) {
        auto* m = activeModel();
        if (!m) return -1;
        m->findAsync(QRegularExpression(pattern,
                                        QRegularExpression::CaseInsensitiveOption));
        for (int guard = 0; m->isComputing() && guard < 2000000; ++guard)
            QCoreApplication::processEvents();
        return m->findMatchCount();
    }
    // Headless mode (window self-test): route would-be modal dialogs to stderr
    // so an offscreen run never blocks on a QMessageBox::exec().
    void setHeadless(bool h) { headless_ = h; }
    // Programmatic region query for the self-test / future scripting: sets
    // the format options of the active tab's file and re-opens it.
    void applyRegionQuery(const QString& region, bool ncbi = false,
                          int slop = 0, bool pileup = false,
                          const QString& tags = QString(), bool gtStats = false,
                          bool contigs = false, const QString& samples = QString(),
                          const QString& matrix = QString()) {
        const OpenFile* f = activeFile();
        if (!f) return;
        reopenFile(f->id, withQuery(f->opts, region, ncbi, slop, pileup, tags, gtStats,
                                    contigs, samples, matrix));
    }

private:
    struct JsonTab;                       // a JSON tree tab (defined below)
    struct OpenFile {                     // an open file and its format options
        int         id = 0;
        QString     path;
        FileOptions opts;
        bool        jsonTree = false;     // a JSON tree tab: nothing to re-open
        FileFeatures feat;                // which format options it offers
    };

    void onCurrentTab(int i) {
        materializeTab(i);   // a tab not shown before: build its model now
        syncHeaderTypes(activeView());
        updateTitle();
        syncFilterCompletions();
        if (filterEdit_) {
            auto* m = activeModel();
            filterEdit_->setText(m ? activeFilterText(m) : QString());
            filterEdit_->setStyleSheet({});
        }
        // Copy and Copy Row act on a table; elsewhere Ctrl+C reaches the view.
        for (QAction* a : {copyAct_, copyRowAct_, exportSelAct_}) if (a) a->setEnabled(activeView() != nullptr);
        for (QAction* a : copyAsActs_) a->setEnabled(activeView() != nullptr);
        if (selTimer_) selTimer_->start();
        if (colTimer_) colTimer_->start();
        syncFileControls();
        refreshStatus();
        refreshColumnsMenu();
        if (activeJsonTab())
            updateDetail({});
        else if (auto* v = activeView())
            updateDetail(v->selectionModel()->currentIndex());
    }
    static std::string src_label(ArrowTableModel* m) {
        return m->tabLabel();
    }

    // Per-pixel scrolling (the default), or Qt's per-item stepping, which
    // snaps the leftmost column and top row to the viewport edge.
    void applyScrollMode(QTableView* view) const {
        const auto mode = smoothScroll_ ? QAbstractItemView::ScrollPerPixel
                                        : QAbstractItemView::ScrollPerItem;
        view->setHorizontalScrollMode(mode);
        view->setVerticalScrollMode(mode);
    }

    // A tab for `src`. Unless `now`, it is a placeholder that keeps the source
    // unread: its model and view are built when the tab is first shown
    // (materializeTab), so opening a file of many tabs reads only the one
    // displayed, and closing a tab never shown reads nothing. Returns the
    // model when built now. `fileId`: the open file it belongs to (files_; 0
    // for none); `at`: the tab index to insert at (-1 appends).
    ArrowTableModel* addSourceTab(std::unique_ptr<TabularSource> src, const QString& origin,
                                  bool primary, const QString& expand = QString(),
                                  std::shared_ptr<SourceOwner> owner = {}, int fileId = 0,
                                  bool now = false, int at = -1) {
        if (!owner) owner = std::make_shared<SourceOwner>();
        if (now) return buildTab(std::move(src), origin, primary, expand, std::move(owner), at, fileId);
        auto* ph = new QWidget(tabs_);
        const QString label = QString::fromStdString(src->tab_label());   // reads no data
        pending_[ph] = PendingTab{std::move(src), origin, primary, expand, std::move(owner), fileId};
        tabFile_[ph] = fileId;
        if (at < 0 || at > tabs_->count()) at = tabs_->count();
        views_.insert(views_.begin() + at, nullptr);
        models_.insert(models_.begin() + at, nullptr);
        tabs_->insertTab(at, ph, label);   // the first tab added becomes current → built
        return nullptr;
    }

    // Build the model and view of placeholder tab `i` in its place.
    void materializeTab(int i, bool makeCurrent = true) {
        QWidget* ph = tabs_->widget(i);
        auto it = pending_.find(ph);
        if (it == pending_.end()) return;
        PendingTab p = std::move(it->second);
        pending_.erase(it);
        tabFile_.remove(ph);
        {
            QSignalBlocker b(tabs_);
            tabs_->removeTab(i);
            views_.erase(views_.begin() + i);
            models_.erase(models_.begin() + i);
            ArrowTableModel* m = buildTab(std::move(p.src), p.origin, p.primary, p.expand,
                                          std::move(p.owner), i, p.fileId);
            if (auto st = pendingState_.find(ph); st != pendingState_.end()) {
                applyViewState(m, views_[(size_t)i], *st);
                pendingState_.erase(st);
            }
            if (makeCurrent) tabs_->setCurrentIndex(i);
        }
        relabelTabs();
        ph->deleteLater();
    }
    // Build every placeholder tab (the self-test's tab dump reports them all).
    void materializeAllTabs() {
        const int cur = tabs_->currentIndex();
        for (int i = 0; i < tabs_->count(); ++i) materializeTab(i, false);
        QSignalBlocker b(tabs_);
        tabs_->setCurrentIndex(cur);
    }

    // Build one model+view tab from a source and wire sort + detail-pane;
    // appended, or inserted at tab index `at`.
    ArrowTableModel* buildTab(std::unique_ptr<TabularSource> src, const QString& origin,
                              bool primary, const QString& expand,
                              std::shared_ptr<SourceOwner> owner, int at, int fileId) {
        auto* model = new ArrowTableModel(std::move(src), this, std::move(owner), /*async=*/true);
        const QFileInfo ofi(origin);
        // A URL can be opened again too (a region query re-opens the files).
        const bool reopenable = origin != QLatin1String("-") &&
            (ofi.isFile() || ofi.isDir() || origin.contains(QLatin1String("://")));
        tabOrigin_[model] = TabOrigin{origin, primary, expand, reopenable};
        auto* view  = new TableView(tabs_);
        auto* header = new TypedHeaderView(view);
        header->setSectionsClickable(true);
        header->setHighlightSections(true);
        header->setShowTypes(showTypes_);
        view->setHorizontalHeader(header);
        // VVG_MIN_ROW_PX: the self-test's stand-in for a style whose header
        // sections have a larger minimum height (Breeze: 30 px).
        static const int testMinRowPx = [] {
            const char* e = std::getenv("VVG_MIN_ROW_PX");
            return e ? std::max(0, std::atoi(e)) : 0;
        }();
        if (testMinRowPx > 0) view->verticalHeader()->setMinimumSectionSize(testMinRowPx);
        // Rows are the font's height plus a margin, unless the style sets a
        // taller minimum (Breeze: 30 px), which setDefaultSectionSize does not
        // go below. The cap keeps rows × that height under Qt's int pixel range.
        view->verticalHeader()->setDefaultSectionSize(view->fontMetrics().height() + 6);
        const int rowPx = std::max(view->verticalHeader()->defaultSectionSize(),
                                   view->verticalHeader()->minimumSectionSize());
        model->setMaxViewRows(std::numeric_limits<int>::max() / rowPx - 1);
        model->setLoadBudget(loadBudget_);
        view->setModel(model);
        view->setAlternatingRowColors(true);
        applyScrollMode(view);
        // A stream's rows arrive as the view scrolls down; the last batch
        // before the load budget brings the Load More row.
        connect(model, &QAbstractItemModel::rowsInserted, this, [this, model] {
            if (model == activeModel()) { syncFileControls(); refreshStatus(); }
        });
        // Size columns to their content once, from the first rows read (the
        // first chunk the view asks for). Nothing resizes them again, so a
        // later manual drag is preserved for the life of the tab.
        auto sized = std::make_shared<QMetaObject::Connection>();
        *sized = connect(model, &ArrowTableModel::chunksLoaded, view, [view, model, sized] {
            if (model->loadedPrefixRows() <= 0) return;
            QObject::disconnect(*sized);
            autosizeColumns(view, model);
        });
        // The schema and the first rows arrive from the owner thread.
        connect(model, &ArrowTableModel::ready, this, [this, model] {
            if (model != activeModel()) return;
            syncFileControls();           // the tab's traits came with its schema
            refreshStatus();
            refreshColumnsMenu();
            if (colTimer_) colTimer_->start();
        });
        connect(model, &ArrowTableModel::chunksLoaded, this, [this, view, model] {
            if (model != activeModel()) return;
            detailRow_ = -1;              // its "…" values may have arrived
            updateDetail(view->selectionModel()->currentIndex());
            if (selTimer_) selTimer_->start();
            refreshStatus();
        });
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view->setSelectionBehavior(QAbstractItemView::SelectItems);
        view->setItemDelegate(new CursorColumnDelegate(view));
        if (model->isReady()) applySequenceDelegates(model, view, fileId);
        else connect(model, &ArrowTableModel::ready, view,
                     [this, model, view, fileId] { applySequenceDelegates(model, view, fileId); });
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(view, &QWidget::customContextMenuRequested, this,
                [this, view](const QPoint& p){ showTableContextMenu(view, p); });
        view->horizontalHeader()->setSectionsClickable(true);

        // Click a column header to sort: ascending, descending, then the
        // file's order again; typed via Arrow, off the UI thread. Shift+click
        // adds the column as a further key (or cycles that key). The cursor
        // moves to that column, so the Column tab summarises the column clicked.
        // The frozen columns' header (TableView's overlay) does the same.
        for (QHeaderView* hh : {view->horizontalHeader(), view->frozenView()->horizontalHeader()}) {
            hh->setSectionsClickable(true);
            connect(hh, &QHeaderView::sectionClicked, this, [this, view, model](int section) {
                keepCol_[model] = section;
                headerSortClick(model, view, section,
                                QGuiApplication::keyboardModifiers() & Qt::ShiftModifier);
            });
            hh->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(hh, &QWidget::customContextMenuRequested, this,
                    [this, view, model, hh](const QPoint& p) { headerMenu(model, view, hh, p); });
        }
        view->frozenView()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(view->frozenView(), &QWidget::customContextMenuRequested, this,
                [this, view](const QPoint& p) { showTableContextMenu(view->frozenView(), p); });

        // A filter, sort or find resets the model twice (blank, then the new
        // order), and a reset drops the view's cursor. Keep its column: the
        // cursor comes back in it, on the top row, once the job is done.
        connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this, view, model] {
            const QModelIndex cur = view->currentIndex();
            if (cur.isValid() && !keepCol_.contains(model)) keepCol_[model] = cur.column();
        });
        connect(model, &QAbstractItemModel::modelReset, this, [this, view, model] {
            if (!model->isComputing() && pendingCursor_.contains(model)) { placePendingCursor(model, view); return; }
            if (model->isComputing() || !keepCol_.contains(model)) return;
            const int col = keepCol_.take(model);
            if (col < model->columnCount() && model->rowCount() > 0)
                view->selectionModel()->setCurrentIndex(
                    model->index(std::max(0, view->rowAt(0)), col),
                    QItemSelectionModel::ClearAndSelect);
        });

        connect(view->selectionModel(), &QItemSelectionModel::currentChanged,
                this, [this, view](const QModelIndex& cur, const QModelIndex& prev) {
                    updateDetail(cur);
                    if (cur.column() != prev.column()) {
                        view->viewport()->update();   // the column tint
                        updateColumnLocation();
                        if (colTimer_) colTimer_->start();
                    }
                });
        // The Column tab says when its column is scrolled out of view.
        connect(view->horizontalScrollBar(), &QScrollBar::valueChanged,
                this, [this] { updateColumnLocation(); });
        connect(view->horizontalHeader(), &QHeaderView::sectionResized,
                this, [this] { updateColumnLocation(); });
        connect(view->selectionModel(), &QItemSelectionModel::selectionChanged,
                selTimer_, [this]{ selTimer_->start(); });
        connect(model, &QAbstractItemModel::modelReset,
                selTimer_, [this]{ selTimer_->start(); });

        // Background filter/sort progress for this model.
        connect(model, &ArrowTableModel::recomputeStarted, this,
                [this, model]{ onRecomputeStarted(model); });
        connect(model, &ArrowTableModel::recomputeFinished, this,
                [this, model](qint64){ onRecomputeFinished(model); });
        connect(model, &ArrowTableModel::recomputeCanceled, this,
                [this]{ onRecomputeStopped(); });
        // A canceled pass over a stream keeps what it read: read the file
        // again to let it go (the tabs keep their view).
        connect(model, &ArrowTableModel::heldAfterCancel, this, [this, fileId](qint64 bytes) {
            // Not inside the model's own signal: the reload deletes it.
            QTimer::singleShot(0, this, [this, fileId, bytes] {
                if (!reloadFile(fileId)) return;
                statusBar()->showMessage(tr("Canceled; the %1 of rows it had read were let go")
                    .arg(QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat)), 6000);
            });
        });
        connect(model, &ArrowTableModel::findReady, this,
                [this, model](qint64 n){ onFindReady(model, n); });

        const QString label = QString::fromStdString(src_label(model));
        tabFile_[view] = fileId;
        if (at < 0) {
            views_.push_back(view);
            models_.push_back(model);
            tabs_->addTab(view, label);
        } else {
            views_.insert(views_.begin() + at, view);
            models_.insert(models_.begin() + at, model);
            tabs_->insertTab(at, view, label);
        }
        return model;
    }

    // Close tab `i`. Its file is closed with its last tab, unless `keepFile`
    // (reopenFile replaces the file's tabs).
    void closeTab(int i, bool keepFile = false) {
        if (i < 0 || i >= tabs_->count()) return;
        // views_/models_ are kept index-aligned with the tab order.
        if (!keepFile) rememberClosedTab(i);
        ArrowTableModel* m = (i < (int)models_.size()) ? models_[i] : nullptr;
        QWidget* w = tabs_->widget(i);
        const int fileId = tabFile_.take(w);
        if (fileId && !keepFile && std::none_of(tabFile_.cbegin(), tabFile_.cend(),
                                                [fileId](int f) { return f == fileId; })) {
            if (const OpenFile* gone = fileById(fileId); gone && watcher_) watcher_->removePath(gone->path);
            stamps_.remove(fileId);
            changedFiles_.remove(fileId);
            files_.erase(std::remove_if(files_.begin(), files_.end(),
                                        [fileId](const OpenFile& f) { return f.id == fileId; }),
                         files_.end());
        }
        // A tab never shown: its source is destroyed on its file's owner
        // thread, where its sibling tabs (sharing its handles) are read.
        pendingState_.remove(w);
        if (auto pit = pending_.find(w); pit != pending_.end()) {
            auto owner = pit->second.owner;
            std::shared_ptr<TabularSource> dead = std::move(pit->second.src);
            owner->post([dead]() mutable { dead.reset(); });
            pending_.erase(pit);
        }
        // Erase this tab's entries first: removing the current tab makes
        // another current, and showing a placeholder for the first time
        // (materializeTab) inserts its model at that tab's index.
        if (i < (int)views_.size()) {
            views_.erase(views_.begin() + i);
            models_.erase(models_.begin() + i);
        }
        tabs_->removeTab(i);
        if (m) { tabOrigin_.remove(m); filterText_.remove(m); statsCache_.remove(m); keepCol_.remove(m); }
        if (m && m == statsModel_) cancelColumnStats();
        jsonTabs_.erase(w);   // a JSON tree tab: its model, document and temporary copy
        if (m == pendingFindModel_) pendingFindModel_ = nullptr;
        if (m == computingModel_) {   // its worker is cancelled+joined in ~ArrowTableModel
            computingModel_ = nullptr;
            if (progress_)  progress_->setVisible(false);
            if (cancelBtn_) cancelBtn_->setVisible(false);
        }
        delete w;   // QTableView (detaches from its model)
        delete m;   // model is parented to MainWindow; free it now
        if (!keepFile) {
            relabelTabs();
            updateTitle();
        }
        refreshStatus();
    }

    // ── Genome Goggles (gg), the genome browser ──────────────────────────────
    // gg's program: on PATH, else beside vvg; "" when neither.
    static QString ggProgram() {
        static const QString found = [] {
            QString p = QStandardPaths::findExecutable(QStringLiteral("gg"));
            if (p.isEmpty())
                p = QStandardPaths::findExecutable(QStringLiteral("gg"), {QCoreApplication::applicationDirPath()});
            return p;
        }();
        return found;
    }
    // How gg takes a file of this format: as a track, as the genome (FASTA,
    // 2bit), or not at all.
    enum class GgRole { None, Track, Genome };
    static GgRole ggRole(const std::string& fmt) {
        static const char* const kTracks[] = {
            "BED", "ENCODE peak / signal", "GFF / GFF3 / GTF", "VCF", "BCF", "BAM / CRAM alignments",
            "SAM alignments (text)", "UCSC bigBed / bigWig", "UCSC wiggle", "LociSSD", "Apache Parquet",
            "Genomics TSV layouts"};
        for (const char* t : kTracks) if (fmt == t) return GgRole::Track;
        if (fmt == "FASTA" || fmt == "UCSC 2bit") return GgRole::Genome;
        return GgRole::None;
    }
    // The BED-style locus (0-based, half-open) of view row `row` of `m`,
    // at least 100 bp wide around it; "" when the tab has no position
    // columns or the row none.
    static QString rowLocus(ArrowTableModel* m, int row, const std::string& fmt) {
        const LocusColumns lc = locus_columns(*m->schema(), fmt);
        if (!lc.ok()) return {};
        auto display = [m](int src) {
            for (int c = 0; c < m->displayColumnCount(); ++c) if (m->sourceColumn(c) == src) return c;
            return -1;
        };
        ArrowTableModel::SyncReads sync(m);
        auto value = [&](int src, bool* ok) {
            const int c = display(src);
            if (c < 0) { *ok = false; return ValueCount{}; }
            ValueCount v = m->filterValue(row, c, ok);
            if (v.null) *ok = false;
            return v;
        };
        bool ok1 = false, ok2 = false;
        const QString chrom = QString::fromStdString(value(lc.chrom, &ok1).value);
        const qint64 start = QString::fromStdString(value(lc.start, &ok2).value).toLongLong(&ok2);
        if (!ok1 || !ok2 || chrom.isEmpty()) return {};
        qint64 end = -1;
        bool ok3 = false;
        if (lc.end >= 0) {
            end = QString::fromStdString(value(lc.end, &ok3).value).toLongLong(&ok3);
            if (!ok3) end = -1;
        } else if (lc.length_from >= 0) {
            const std::string t = value(lc.length_from, &ok3).value;
            if (ok3 && t != "*" && !t.empty()) end = start + (qint64)t.size() - (lc.one_based ? 1 : 0);
        }
        // To BED-style: a 1-based inclusive end is the half-open end.
        qint64 b = lc.one_based ? start - 1 : start;
        qint64 e = end >= 0 ? end : b + 1;
        if (e <= b) e = b + 1;
        if (e - b < 100) {   // some context: a short feature centred in 100 bp
            b = std::max<qint64>(0, b - (100 - (e - b)) / 2);
            e = b + 100;
        }
        return QStringLiteral("%1:%2-%3").arg(chrom).arg(b).arg(e);
    }
    // gg's arguments for file `f`, at `locus` (BED-style) when given, else
    // at the file's Region when it has one.
    static QStringList ggArguments(const OpenFile& f, const QString& locus) {
        QStringList a;
        if (!locus.isEmpty()) a << QStringLiteral("--coords") << QStringLiteral("bed") << QStringLiteral("-l") << locus;
        else if (!f.opts.region.empty())
            a << QStringLiteral("--coords") << (f.opts.ncbi ? QStringLiteral("one") : QStringLiteral("bed"))
              << QStringLiteral("-l") << QString::fromStdString(f.opts.region).section(QLatin1Char(','), 0, 0);
        if (ggRole(f.feat.format) == GgRole::Genome) a << QStringLiteral("-g");
        a << f.path;
        return a;
    }
    // "Open in Genome Goggles" for file `f` (at `locus`), when gg is there
    // and reads the file; nullptr otherwise.
    QAction* addGgAction(QMenu& menu, const OpenFile* f, const QString& locus) {
        if (!f || f->jsonTree || ggRole(f->feat.format) == GgRole::None || ggProgram().isEmpty()) return nullptr;
        QAction* a = menu.addAction(locus.isEmpty() ? tr("Open in &Genome Goggles")
                                                    : tr("Open in &Genome Goggles at %1").arg(locus));
        a->setToolTip(tr("Start gg, the genome browser, with this file") +
                      (locus.isEmpty() ? QString() : tr(" at this row's position")));
        const QStringList args = ggArguments(*f, locus);
        connect(a, &QAction::triggered, this, [this, args] {
            if (!QProcess::startDetached(ggProgram(), args))
                statusBar()->showMessage(tr("Could not start %1").arg(ggProgram()), 5000);
        });
        return a;
    }

    // Sequence and quality columns of alignment, FASTA / FASTQ and variant
    // files get coloured bases / quality backgrounds (SequenceDelegate).
    void applySequenceDelegates(ArrowTableModel* m, QTableView* v, int fileId) {
        const OpenFile* f = fileById(fileId);
        if (!f) return;
        const std::string& fmt = f->feat.format;
        const bool reads = fmt == "BAM / CRAM alignments" || fmt == "SAM alignments (text)" ||
                           fmt == "FASTA" || fmt == "FASTQ";
        const bool variants = fmt == "VCF" || fmt == "BCF";
        if (!reads && !variants) return;
        for (int c = 0; c < m->columnCount(); ++c) {
            const QString name = m->columnName(c), type = m->columnType(c);
            if (type != QLatin1String("string") && type != QLatin1String("large_string")) continue;
            const bool seq = reads ? (name == QLatin1String("SEQ") || name == QLatin1String("seq") ||
                                      name == QLatin1String("sequence"))
                                   : (name == QLatin1String("REF") || name == QLatin1String("ALT"));
            const bool qual = reads && fmt != "FASTA" &&
                              (name == QLatin1String("QUAL") || name == QLatin1String("qual"));
            if (seq || qual)
                v->setItemDelegateForColumn(c, new SequenceDelegate(seq ? SequenceDelegate::Kind::Bases
                                                                        : SequenceDelegate::Kind::Quality, v));
        }
    }

    // A column header's right-click menu (`h`: the table's header or the
    // frozen columns').
    void headerMenu(ArrowTableModel* model, TableView* view, QHeaderView* h, const QPoint& p) {
        const int section = h->logicalIndexAt(p);
        QMenu menu(this);
        if (section >= 0) {
            connect(menu.addAction(tr("Sort &Ascending")), &QAction::triggered, this,
                    [=, this] { sortByColumn(model, view, section, Qt::AscendingOrder); });
            connect(menu.addAction(tr("Sort &Descending")), &QAction::triggered, this,
                    [=, this] { sortByColumn(model, view, section, Qt::DescendingOrder); });
        }
        if (section >= 0 && !model->sortKeys().empty() && model->sortColumn() != section) {
            connect(menu.addAction(tr("Then Sort Ascending")), &QAction::triggered, this,
                    [=, this] { addSortKey(model, view, section, Qt::AscendingOrder); });
            connect(menu.addAction(tr("Then Sort Descending")), &QAction::triggered, this,
                    [=, this] { addSortKey(model, view, section, Qt::DescendingOrder); });
        }
        QAction* clr = menu.addAction(tr("Clear &Sort"));
        clr->setEnabled(model->sortColumn() >= 0);
        connect(clr, &QAction::triggered, this, [=, this] { clearSortOf(model, view); });
        if (section >= 0) {
            menu.addSeparator();
            connect(menu.addAction(tr("&Hide Column")), &QAction::triggered, this,
                    [=, this] { view->setColumnHidden(section, true); refreshColumnsMenu(); });
            // Coloured bases / qualities can be turned off for the column.
            if (auto* sd = dynamic_cast<SequenceDelegate*>(view->itemDelegateForColumn(section))) {
                QAction* plain = menu.addAction(sd->kind() == SequenceDelegate::Kind::Bases
                                                    ? tr("Plain Bases") : tr("Plain Qualities"));
                connect(plain, &QAction::triggered, this, [view, section] {
                    view->setItemDelegateForColumn(section, nullptr);
                    view->viewport()->update();
                });
            }
            // Freeze the visible columns up to this one (on-screen order).
            int upTo = 0;
            for (int vis = 0; vis <= view->horizontalHeader()->visualIndex(section); ++vis)
                upTo += !view->isColumnHidden(view->horizontalHeader()->logicalIndex(vis));
            connect(menu.addAction(upTo == 1 ? tr("&Freeze This Column") : tr("&Freeze Columns up to Here")),
                    &QAction::triggered, this, [view, upTo] { view->setFrozenColumns(upTo); });
            if (view->frozenColumns() > 0)
                connect(menu.addAction(tr("&Unfreeze Columns")), &QAction::triggered, this,
                        [view] { view->setFrozenColumns(0); });
            // The field reference's entry for this column, when it has one.
            const QString name = model->headerData(section, Qt::Horizontal, Qt::DisplayRole).toString();
            if (const QString anchor = HelpDialog::anchorFor(name); !anchor.isEmpty())
                connect(menu.addAction(tr("Field Reference: %1").arg(name)), &QAction::triggered,
                        this, [this, anchor] { HelpDialog::showReference(this, anchor); });
        }
        menu.exec(h->mapToGlobal(p));
    }

    // Close every tab of file `id`.
    void closeFile(int id) {
        for (int i = tabs_->count() - 1; i >= 0; --i)
            if (tabFile_.value(tabs_->widget(i), 0) == id) closeTab(i);
    }
    int tabsOfFile(int id) const {
        int n = 0;
        for (int i = 0; i < tabs_->count(); ++i) n += tabFile_.value(tabs_->widget(i), 0) == id;
        return n;
    }
    int filesWithTabs() const {
        QSet<int> ids;
        for (int i = 0; i < tabs_->count(); ++i)
            if (int id = tabFile_.value(tabs_->widget(i), 0)) ids.insert(id);
        return ids.size();
    }

    // File `id`'s colour: one of eight hues, lighter on a dark palette.
    QColor fileColor(int id) const {
        static const int kHues[] = {210, 30, 130, 280, 0, 175, 55, 330};
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        return QColor::fromHsl(kHues[(id - 1 + 8) % 8], 160, dark ? 160 : 105);
    }

    // The tab texts: a tab's own label, and with two files or more open the
    // first tab of a file of several tabs also names the file
    // ("adata.h5ad ▸ summary"). Each tab's tooltip is its file's path.
    void relabelTabs() {
        const bool several = filesWithTabs() >= 2;
        int prev = -1;
        for (int i = 0; i < tabs_->count(); ++i) {
            const int id = tabFile_.value(tabs_->widget(i), 0);
            const QString label = tabs_->label(i);
            const OpenFile* f = id ? fileById(id) : nullptr;
            QString text = label;
            if (f && several && id != prev && tabsOfFile(id) > 1)
                text = QFileInfo(f->path).fileName() + QStringLiteral(" \u25B8 ") + label;
            if (tabs_->tabText(i) != text) tabs_->setTabText(i, text);
            if (f) tabs_->setTabToolTip(i, label == QFileInfo(f->path).fileName()
                                               ? f->path : f->path + QLatin1Char('\n') + label);
            prev = id;
        }
        tabs_->tabBar()->update();
        syncFileControls();
    }

    // The window title: the current tab's file.
    void updateTitle() {
        const OpenFile* f = activeFile();
        if (f) setWindowTitle(f->path + QStringLiteral(" — vv"));
        else if (tabs_->count() > 0) setWindowTitle(tabs_->tabText(tabs_->currentIndex()) + QStringLiteral(" — vv"));
        else setWindowTitle(QStringLiteral("vv"));
    }

    // Right-click on the tab bar: close the tab, its file or the other files;
    // copy the file's path.
    void tabMenu(const QPoint& pos) {
        const int i = tabs_->tabBar()->tabAt(pos);
        if (i < 0) return;
        QWidget* w = tabs_->widget(i);
        const int id = tabFile_.value(w, 0);
        const OpenFile* f = id ? fileById(id) : nullptr;
        QMenu menu(this);
        connect(menu.addAction(tr("Close Tab")), &QAction::triggered, this,
                [this, w] { closeTab(tabs_->indexOf(w)); });
        if (f) {
            const int n = tabsOfFile(id);
            QAction* cf = menu.addAction(n > 1 ? tr("Close File (%1 tabs)").arg(n) : tr("Close File"));
            connect(cf, &QAction::triggered, this, [this, id] { closeFile(id); });
            QAction* co = menu.addAction(tr("Close Other Files"));
            co->setEnabled(filesWithTabs() > 1);
            connect(co, &QAction::triggered, this, [this, id] {
                QList<int> others;
                for (const auto& o : files_) if (o.id != id) others << o.id;
                for (int o : others) closeFile(o);
            });
            menu.addSeparator();
            const QString path = f->path;
            connect(menu.addAction(tr("Copy Path")), &QAction::triggered, this,
                    [path] { QGuiApplication::clipboard()->setText(path); });
            addGgAction(menu, f, {});
        }
        menu.addSeparator();
        menu.addAction(reopenClosedAct_);
        menu.exec(tabs_->tabBar()->mapToGlobal(pos));
    }

    // ── Actions, menus, toolbar (the layout of gg, the genome browser) ──────
    // Every command is one QAction, added to the window so its shortcut works
    // with the menu bar hidden; the menu bar and the ≡ menu hold the same
    // actions and submenus. Tooltips read "Name (Shortcut)\nexplanation".
    QAction* makeAction(const QString& text, const QKeySequence& key, const QString& explanation,
                        std::function<void()> fn = {}) {
        auto* a = new QAction(text, this);
        if (!key.isEmpty()) a->setShortcut(key);
        if (fn) connect(a, &QAction::triggered, this, [fn = std::move(fn)] { fn(); });
        return reg(a, explanation);
    }
    QAction* reg(QAction* a, const QString& explanation) {
        addAction(a);
        QString name = a->text();
        name.remove(QLatin1Char('&'));
        if (name.endsWith(QChar(0x2026))) name.chop(1);
        QString tip = name;
        if (!a->shortcut().isEmpty())
            tip += QStringLiteral(" (%1)").arg(a->shortcut().toString(QKeySequence::NativeText));
        if (!explanation.isEmpty()) tip += QLatin1Char('\n') + explanation;
        a->setToolTip(tip);
        return a;
    }
    // The theme's icon, else the drawn glyph; redrawn when the palette changes.
    QIcon themedIcon(const char* themeName, Glyph g) const {
        if (*themeName && QIcon::hasThemeIcon(QLatin1String(themeName)))
            return QIcon::fromTheme(QLatin1String(themeName));
        return glyphIcon(g, palette().color(QPalette::WindowText));
    }
    void setActionIcon(QAction* a, const char* themeName, Glyph g) {
        iconActions_.push_back({a, themeName, g});
        a->setIcon(themedIcon(themeName, g));
    }
    void refreshIcons() {
        for (const auto& ia : iconActions_) ia.action->setIcon(themedIcon(ia.themeName, ia.glyph));
    }

    void buildMenus() {
        openAct_ = makeAction(tr("&Open…"), QKeySequence::Open, tr("Open one or more data files"),
                                   [this] { openFilesDialog(); });
        openDirAct_ = makeAction(tr("Open &Directory…"), {},
            tr("Open a directory as one dataset (Parquet / CSV / JSON parts; Hive key=value/ partitions)"),
            [this] { openDirectoryDialog(); });
        exportAction_ = makeAction(tr("&Export View As…"), QKeySequence(QStringLiteral("Ctrl+E")),
            tr("Write this tab's rows — with its filter, sort and visible columns — to Parquet, "
               "Arrow, TSV, CSV or JSON"),
            [this] { exportViewDialog(); });
        exportAction_->setIconText(tr("Export…"));
        setActionIcon(exportAction_, "document-export", Glyph::Export);
        openExportDirAct_ = makeAction(tr("Open Export &Directory"), {},
            tr("Show the directory of the last export"), [this] {
                if (!exportPath_.isEmpty())
                    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(exportPath_).absolutePath()));
            });
        openExportDirAct_->setEnabled(false);
        QAction* close = makeAction(tr("&Close Tab"), QKeySequence::Close, {},
                                    [this] { closeTab(tabs_->currentIndex()); });
        closeFileAct_ = makeAction(tr("Close &File"), QKeySequence(QStringLiteral("Ctrl+Shift+W")),
            tr("Close every tab of the current tab's file"), [this] {
                if (int id = tabFile_.value(tabs_->currentWidget(), 0)) closeFile(id);
                else closeTab(tabs_->currentIndex());
            });
        QAction* quit = makeAction(tr("&Quit"), QKeySequence::Quit, {}, [] { QApplication::quit(); });

        copyAct_ = makeAction(tr("&Copy"), QKeySequence::Copy, tr("Copy the selected cells"),
                                 [this] { copySelection(); });
        copyRowAct_ = makeAction(tr("Copy &Row"), QKeySequence(QStringLiteral("Ctrl+Shift+C")),
                                    tr("Copy the selected rows, every column"), [this] { copyRows(); });
        // Edit ▸ Copy As: the selection with a header row, as CSV or a Markdown table.
        copyAsMenu_ = new QMenu(tr("Copy &As"), this);
        copyAsMenu_->setToolTipsVisible(true);
        for (auto [text, fmt, tip] : std::initializer_list<std::tuple<QString, CopyFormat, QString>>{
                 {tr("&TSV with Header"), CopyFormat::TsvHeader, tr("Tab-separated, the column names first")},
                 {tr("&CSV"), CopyFormat::Csv, tr("Comma-separated with a header row, quoted as RFC 4180")},
                 {tr("&Markdown Table"), CopyFormat::Markdown, tr("A GitHub-flavoured Markdown table")}}) {
            QAction* a = makeAction(text, {}, tip, [this, fmt] { copySelectionAs(fmt, false); });
            copyAsMenu_->addAction(a);
            copyAsActs_ << a;
        }
        openUrlAct_ = makeAction(tr("Open &URL…"), QKeySequence(QStringLiteral("Ctrl+Shift+O")),
            tr("Open a file over https, s3, gs or ftp (read through htslib)"), [this] { openUrlDialog(); });
        if (remote_schemes() == "none") {
            openUrlAct_->setEnabled(false);
            openUrlAct_->setToolTip(tr("This build of vvg reads no URLs: its htslib has no libcurl"));
        }
        exportSelAct_ = makeAction(tr("Export &Selection As…"), {},
            tr("Write the selected rows and columns (in the view's order) to Parquet, Arrow, TSV, "
               "CSV or JSON"), [this] { exportViewDialog(true); });
        openAct_->setIconText(tr("Open"));
        setActionIcon(openAct_, "document-open", Glyph::Open);
        setActionIcon(openDirAct_, "folder-open", Glyph::Directory);
        setActionIcon(copyAct_, "edit-copy", Glyph::Copy);
        setActionIcon(copyRowAct_, "", Glyph::CopyRow);
        copyAct_->setEnabled(false);       // until a table tab is shown
        copyRowAct_->setEnabled(false);
        copyCmdAct_ = makeAction(tr("Copy as &vv Command"), QKeySequence(QStringLiteral("Ctrl+Alt+C")),
            tr("Copy the vv command line that reproduces this tab: file, tab, format options, "
               "filter, sort and the visible columns"),
            [this] { copyCommand(); });
        // F3 alone: QKeySequence::FindNext also carries Ctrl+G on GNOME /
        // generic themes, which would collide with Go to Row. Two window
        // shortcuts on one key are ambiguous and Qt runs neither.
        QAction* focusRegion = makeAction(tr("&Region…"), QKeySequence(QStringLiteral("Ctrl+L")),
            tr("Type a region, e.g. chr1:1,000-2,000, on a tab whose file takes one"),
            [this] { contextBar_->focusRegion(); });
        QAction* findNext = makeAction(tr("Find &Next"), QKeySequence(Qt::Key_F3), tr("The next match of Find"),
                                       [this] { doFind(true); });
        QAction* focusFilter = makeAction(tr("&Filter…"), QKeySequence(QStringLiteral("Ctrl+Shift+F")),
            tr("Type a row filter, e.g. score > 5 and chrom == \"chr1\""),
            [this] { filterEdit_->setFocus(); filterEdit_->selectAll(); });
        QAction* focusFind = makeAction(tr("F&ind…"), QKeySequence(QStringLiteral("Ctrl+F")),
            tr("Type a regular expression to find in the cells"),
            [this] { findEdit_->setFocus(); findEdit_->selectAll(); });

        QAction* go = makeAction(tr("&Go to Row…"), QKeySequence(QStringLiteral("Ctrl+G")), {},
                                 [this] { gotoRow(); });
        QAction* clearSort = makeAction(tr("Clear &Sort"), {},
            tr("Back to the file's row order (a third click on the sorted header does the same)"),
            [this] { if (auto* m = activeModel()) clearSortOf(m, activeView()); });
        // Smooth (per-pixel) scrolling in both directions; off = Qt's default
        // per-item stepping, which keeps the leftmost column / top row aligned
        // to the edge. Saved in vvg's settings (key smoothScroll).
        QAction* smooth = makeAction(tr("&Smooth Scrolling"), {},
            tr("Scroll by pixels; off steps whole columns and rows"));
        smooth->setCheckable(true);
        smooth->setChecked(smoothScroll_);
        connect(smooth, &QAction::toggled, this, [this](bool on) {
            smoothScroll_ = on;
            saveSetting(QStringLiteral("smoothScroll"), on);
            for (auto* v : views_) if (v) applyScrollMode(v);
        });
        reopenClosedAct_ = makeAction(tr("Reopen Closed &Tab"), QKeySequence(QStringLiteral("Ctrl+Shift+T")),
            tr("Open the last closed tab again, with its filter, sort and columns"),
            [this] { reopenClosedTab(); });
        reopenClosedAct_->setEnabled(false);
        restoreSessionAct_ = makeAction(tr("Restore Session at &Start"), {},
            tr("When vvg starts without files, reopen the files, tabs and views of the last window"));
        restoreSessionAct_->setCheckable(true);
        restoreSessionAct_->setChecked(restoreSession_);
        connect(restoreSessionAct_, &QAction::toggled, this, [this](bool on) {
            restoreSession_ = on;
            saveSetting(QStringLiteral("ui/restoreSession"), on);
        });
        loadMoreAct_ = makeAction(tr("&Load More Rows"), {},
            tr("Read the next rows of a large file into memory; vvg stops at a limit so that scrolling cannot fill it"),
            [this] { loadMoreRows(); });
        loadMoreAct_->setEnabled(false);
        QAction* limitsAct = makeAction(tr("&Memory Limits…"), {},
            tr("How many rows a large file loads as you scroll, and when a sort, filter or find asks first"),
            [this] { editMemoryLimits(); });
        reloadAct_ = makeAction(tr("&Reload"), QKeySequence(Qt::Key_F5),
            tr("Read the current tab's file again; each tab keeps its filter, sort and columns"),
            [this] {
                if (const OpenFile* f = activeFile(); f && !reloadFile(f->id) && f->jsonTree)
                    statusBar()->showMessage(tr("A JSON tree is not reloaded; close and open it again"), 4000);
            });
        typesAct_ = makeAction(tr("Column &Types"), {},
            tr("Each column's type under its name, in a smaller, dimmer font"));
        typesAct_->setCheckable(true);
        typesAct_->setChecked(showTypes_);
        connect(typesAct_, &QAction::toggled, this, [this](bool on) {
            showTypes_ = on;
            saveSetting(QStringLiteral("ui/showColumnTypes"), on);
            syncHeaderTypes(activeView());   // the other tabs when they are shown
        });
        showMenuBarAct_ = makeAction(tr("&Menu Bar"), {},
            tr("The classic File / Edit / View / Help bar; the ≡ button holds the same"));
        showMenuBarAct_->setCheckable(true);
        connect(showMenuBarAct_, &QAction::toggled, this, [this](bool on) {
            if (menuBar()->isNativeMenuBar()) return;
            menuBar()->setVisible(on);
            saveSetting(QStringLiteral("ui/showMenuBar"), on);
        });
        showAppMenuAct_ = makeAction(tr("Me&nu"), QKeySequence(Qt::Key_F10),
                                     tr("Files, export, edit, view and help"), [this] {
            if (appButton_ && appButton_->isVisible()) appButton_->showMenu();
            else if (appMenu_) appMenu_->popup(mainBar_->mapToGlobal(QPoint(mainBar_->width() - 1, mainBar_->height())));
        });
        setActionIcon(showAppMenuAct_, "open-menu", Glyph::Menu);
        QAction* referenceAct = makeAction(tr("Field &Reference…"), QKeySequence(Qt::Key_F1),
            tr("How to use vvg, the Filter / Find / Region syntax, and what the columns and keys "
               "of AnnData, VCF, BED, SAM and other files mean"),
            [this] { HelpDialog::showReference(this); });
        QAction* shortcutsAct = makeAction(tr("&Keyboard Shortcuts…"), QKeySequence(QStringLiteral("Ctrl+/")),
            tr("Every command with its key"),
            [this] { ShortcutsDialog::showFor(this, commandsForHelp()); });
        QAction* paletteAct = makeAction(tr("Find &Action…"), QKeySequence(QStringLiteral("Ctrl+Shift+P")),
            tr("Run any command by typing part of its name, key or menu"), [this] { openPalette(); });
        QAction* about = makeAction(tr("&About vv"), {}, {}, [this] {
            QMessageBox::about(this, tr("About vv"),
                tr("<p><b>Vertical Viewer (vv)</b> %1<br>"
                   "vvg, its graphical viewer</p>"
                   "<p>Opens every format the vv command line reads (Parquet, Arrow, BAM / CRAM, "
                   "VCF / BCF, BED / GFF, HDF5 / AnnData, Zarr, NumPy, xlsx / ods, SQLite, …) "
                   "through the shared libvvcore reader core. <code>vh</code>, the viewer, "
                   "horizontal, is the same program printing a transposed preview.</p>"
                   "<p>© Piotr Balwierz — MIT licence<br>"
                   "<a href=\"https://github.com/balwierz/vv\">github.com/balwierz/vv</a><br>"
                   "Cite: <a href=\"https://doi.org/%2\">doi:%2</a> (<code>vv --cite</code>)</p>")
                    .arg(QString::fromLatin1(vv_version()), QString::fromLatin1(vv_doi())));
        });
        about->setMenuRole(QAction::AboutRole);

        auto* file = menuBar()->addMenu(tr("&File"));
        file->setToolTipsVisible(true);
        file->addAction(openAct_);
        file->addAction(openDirAct_);
        file->addAction(openUrlAct_);
        recentMenu_ = file->addMenu(tr("Open &Recent"));
        rebuildRecentMenu();
        file->addSeparator();
        file->addAction(exportAction_);
        file->addAction(exportSelAct_);
        file->addSeparator();
        file->addAction(close);
        file->addAction(closeFileAct_);
        file->addAction(reopenClosedAct_);
        file->addSeparator();
        file->addAction(quit);

        auto* edit = menuBar()->addMenu(tr("&Edit"));
        edit->setToolTipsVisible(true);
        edit->addAction(copyAct_);
        edit->addAction(copyRowAct_);
        edit->addMenu(copyAsMenu_);
        edit->addSeparator();
        edit->addAction(copyCmdAct_);
        edit->addSeparator();
        edit->addAction(focusFilter);
        edit->addAction(focusFind);
        edit->addAction(focusRegion);
        edit->addAction(findNext);

        auto* view = menuBar()->addMenu(tr("&View"));
        view->setToolTipsVisible(true);
        viewMenu_ = view;
        view->addAction(go);
        view->addAction(reloadAct_);
        view->addAction(loadMoreAct_);
        view->addAction(limitsAct);
        view->addSeparator();
        columnsMenu_ = view->addMenu(tr("&Columns"));
        refreshColumnsMenu();
        view->addAction(clearSort);
        view->addSeparator();
        view->addAction(detailsAct_);
        view->addAction(typesAct_);
        view->addAction(smooth);
        view->addAction(restoreSessionAct_);
        view->addSeparator();
        view->addAction(showMenuBarAct_);

        auto* help = menuBar()->addMenu(tr("&Help"));
        help->setToolTipsVisible(true);
        help->addAction(referenceAct);
        help->addAction(shortcutsAct);
        help->addAction(paletteAct);
        help->addSeparator();
        help->addAction(about);

        // The menu bar is hidden unless asked for; a native one (macOS) always shows.
        const bool native = menuBar()->isNativeMenuBar();
        showMenuBarAct_->setEnabled(!native);
        showMenuBarAct_->setChecked(native || QSettings(QStringLiteral("vv"), QStringLiteral("vvg"))
                                                  .value(QStringLiteral("ui/showMenuBar"), false).toBool());
        menuBar()->setVisible(showMenuBarAct_->isChecked());

        // The ≡ menu: the same actions and submenus.
        appMenu_ = new QMenu(tr("vvg"), this);
        appMenu_->setToolTipsVisible(true);
        appMenu_->addAction(openAct_);
        appMenu_->addAction(openDirAct_);
        appMenu_->addAction(openUrlAct_);
        appMenu_->addMenu(recentMenu_);
        appMenu_->addSeparator();
        appMenu_->addAction(exportAction_);
        appMenu_->addAction(exportSelAct_);
        appMenu_->addAction(close);
        appMenu_->addAction(closeFileAct_);
        appMenu_->addAction(reopenClosedAct_);
        appMenu_->addSeparator();
        appMenu_->addMenu(edit);
        appMenu_->addMenu(view);
        appMenu_->addSeparator();
        appMenu_->addMenu(help);
        appMenu_->addSeparator();
        appMenu_->addAction(quit);

        exportMenu_ = new QMenu(this);
        exportMenu_->setToolTipsVisible(true);
        exportMenu_->addAction(exportAction_);
        exportMenu_->addAction(exportSelAct_);
        exportMenu_->addAction(copyCmdAct_);
        exportMenu_->addSeparator();
        exportMenu_->addAction(openExportDirAct_);
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
        // A stream's rows count up as they are read: say the range there is.
        const QString label = m->totalRows() < 0 && m->sortKeys().empty() && !m->hasFilter()
            ? tr("Row (1 to %1, the rows read so far):").arg(QLocale().toString(m->rowCount()))
            : tr("Row (1-based):");
        int row = QInputDialog::getInt(this, tr("Go to row"), label, 1, 1, m->rowCount(), 1, &ok);
        if (ok) goToRow(v, row - 1);
    }
    static void goToRow(QTableView* v, int row) {
        const QModelIndex idx = v->model()->index(row, 0);
        v->setCurrentIndex(idx);
        v->scrollTo(idx, QAbstractItemView::PositionAtTop);
    }

    // The Open dialog's types, from libvvcore's format table: every
    // supported pattern, then one entry per family (the underlying format:
    // Parquet / Arrow / ORC, HDF5, …) in alphabetical order, then all files.
    static QStringList openNameFilters() {
        QStringList families;
        QHash<QString, QStringList> globs;
        for (const FormatInfo& f : format_table()) {
            const QString fam = QString::fromUtf8(f.family);
            if (!globs.contains(fam)) families << fam;
            for (const std::string& g : format_globs(f)) globs[fam] << QString::fromStdString(g);
        }
        std::sort(families.begin(), families.end(), [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
        QStringList all, out;
        for (const QString& fam : families) all << globs[fam];
        out << tr("All supported files (%1)").arg(all.join(QLatin1Char(' ')));
        for (const QString& fam : families)
            out << QStringLiteral("%1 (%2)").arg(fam, globs[fam].join(QLatin1Char(' ')));
        out << tr("All files (*)");
        return out;
    }
    void openFilesDialog() {
        const QStringList filters = openNameFilters();
        QString chosen = filters.contains(openFilter_) ? openFilter_ : filters.first();
        QStringList files = QFileDialog::getOpenFileNames(
            this, tr("Open data file(s)"), lastDir_, filters.join(QStringLiteral(";;")), &chosen,
            QFileDialog::HideNameFilterDetails);
        if (!files.isEmpty() && chosen != openFilter_) {
            openFilter_ = chosen;
            saveSetting(QStringLiteral("ui/openFilter"), chosen);
        }
        openPaths(files);
    }

    // A directory opens as one dataset (libvvcore concatenates its part files
    // and turns Hive key=value/ path components into columns).
    // File ▸ Open URL: a URL this build reads (https, s3, …), prefilled from
    // the clipboard when it holds one.
    void openUrlDialog() {
        QString guess = QGuiApplication::clipboard()->text().trimmed();
        if (guess.contains(QLatin1Char('\n')) || !is_remote_url(guess.toStdString())) guess.clear();
        bool ok = false;
        const QString url = QInputDialog::getText(
            this, tr("Open URL"),
            tr("URL of a file (this build reads %1):").arg(QString::fromStdString(remote_schemes())),
            QLineEdit::Normal, guess, &ok).trimmed();
        if (!ok || url.isEmpty()) return;
        if (!remote_supported(url.toStdString())) {
            QMessageBox::warning(this, tr("Open URL"),
                                 tr("This build cannot open %1 URLs; it reads %2.")
                                     .arg(QUrl(url).scheme().isEmpty() ? url : QUrl(url).scheme() + QStringLiteral("://"),
                                          QString::fromStdString(remote_schemes())));
            return;
        }
        openPaths({url});
    }
    void openDirectoryDialog() {
        QString dir = QFileDialog::getExistingDirectory(
            this, tr("Open directory as dataset"), lastDir_);
        if (!dir.isEmpty()) openPaths(QStringList{dir});
    }

    void addRecent(const QString& path) {
        const QString abs = path.contains(QLatin1String("://")) ? path
                                                                : QFileInfo(path).absoluteFilePath();
        recent_.removeAll(abs);
        recent_.prepend(abs);
        while (recent_.size() > 10) recent_.removeLast();
        saveSetting(QStringLiteral("recentFiles"), recent_);
        rebuildRecentMenu();
    }
    void rebuildRecentMenu() {
        if (!recentMenu_) return;
        recentMenu_->clear();
        const QStringList recent = recent_;
        if (recent.isEmpty()) {
            recentMenu_->addAction(tr("(none)"))->setEnabled(false);
            return;
        }
        for (const QString& p : recent)
            connect(recentMenu_->addAction(p), &QAction::triggered, this,
                    [this, p]{ openPaths({p}); });
        recentMenu_->addSeparator();
        connect(recentMenu_->addAction(tr("Clear Recent")), &QAction::triggered, this, [this]{
            recent_.clear();
            saveSetting(QStringLiteral("recentFiles"), {});
            rebuildRecentMenu();
        });
    }

    // Open `path` with its format options, canonicalising the region first.
    // Errors are collected, not thrown.
    static LoadResult loadFile(const QString& path, const FileOptions& opts) {
        Config base = opts.config();
        std::string err = apply_region_modifiers(base);
        if (!err.empty()) {
            LoadResult r; r.errors << QString::fromStdString(err); return r;
        }
        return loadSources(base, {path}, opts.expand);
    }

    static FileOptions withQuery(FileOptions o, const QString& region, bool ncbi, int slop,
                                 bool pileup, const QString& tags, bool gtStats, bool contigs,
                                 const QString& samples, const QString& matrix) {
        o.region  = region.trimmed().toStdString();
        o.ncbi    = ncbi;
        o.slop    = slop;
        o.pileup  = pileup;
        o.tags    = tags.trimmed().toStdString();
        o.gtStats = gtStats;
        o.contigs = contigs;
        if (!samples.isEmpty()) o.samples = samples.toStdString();
        if (!matrix.isEmpty())  o.matrix  = matrix.toStdString();
        return o;
    }

    // Re-open file `id` with `opts`, its new tabs where its old ones were;
    // the other files' tabs keep their filters, sorts and columns. When the
    // file does not open with `opts` (a region on an un-indexed file, …) its
    // tabs and options stay and the reason goes to the status bar.
    // The frozen leading columns of a table view (TableView).
    static int frozenColumns(QTableView* v) {
        auto* t = dynamic_cast<TableView*>(v);
        return t ? t->frozenColumns() : 0;
    }
    static void setFrozenColumns(QTableView* v, int n) {
        if (auto* t = dynamic_cast<TableView*>(v)) t->setFrozenColumns(n);
    }
    // The view state of tab `i` (a tab never shown: the state it is waiting
    // to be given, or none).
    TabViewState captureViewState(int i) const {
        QWidget* w = tabs_->widget(i);
        if (auto st = pendingState_.find(w); st != pendingState_.end()) return *st;
        TabViewState s;
        s.label = tabs_->label(i);
        ArrowTableModel* m = i < (int)models_.size() ? models_[(size_t)i] : nullptr;
        QTableView* v = i < (int)views_.size() ? views_[(size_t)i] : nullptr;
        if (!m || !v || !m->isReady()) return s;
        if (m->hasFilter()) s.filter = filterText_.value(m);
        for (const auto& k : m->sortKeys())
            s.sort.append({m->columnName(k.col), k.order == Qt::DescendingOrder});
        auto* h = v->horizontalHeader();
        bool reordered = false;
        QStringList order;
        for (int vis = 0; vis < m->columnCount(); ++vis) {
            const int c = h->logicalIndex(vis);
            if (c != vis) reordered = true;
            order << m->columnName(c);
            if (v->isColumnHidden(c)) s.hidden << m->columnName(c);
        }
        if (reordered) s.order = order;
        s.frozen = frozenColumns(v);
        const QModelIndex cur = v->currentIndex();
        if (cur.isValid()) {
            s.curColumn = m->columnName(cur.column());
            s.curRow = cur.row();
        }
        return s;
    }
    // Give tab (m, v) a view state: columns hidden and ordered at once, the
    // filter and sort as one recompute, the cursor once its rows are there.
    // A model still reading its schema gets it when ready.
    void applyViewState(ArrowTableModel* m, QTableView* v, TabViewState st) {
        if (!m || !v || st.isDefault()) return;
        if (!m->isReady()) {
            auto* once = new QMetaObject::Connection;
            *once = connect(m, &ArrowTableModel::ready, this, [this, m, v, st, once] {
                QObject::disconnect(*once);
                delete once;
                applyViewState(m, v, st);
            });
            return;
        }
        auto colOf = [m](const QString& name) {
            for (int c = 0; c < m->columnCount(); ++c)
                if (m->columnName(c) == name) return c;
            return -1;
        };
        auto* h = v->horizontalHeader();
        for (int i = 0; i < st.order.size(); ++i)
            if (int c = colOf(st.order[i]); c >= 0 && i < m->columnCount()) h->moveSection(h->visualIndex(c), i);
        for (const QString& name : st.hidden)
            if (int c = colOf(name); c >= 0) v->setColumnHidden(c, true);
        if (st.frozen > 0) setFrozenColumns(v, st.frozen);
        bool recompute = false;
        // A filter or sort reads every row: on a large file it is not
        // re-applied unasked (at start, on Reload or Reopen Closed Tab).
        if ((!st.filter.isEmpty() || !st.sort.isEmpty()) &&
            m->fullPassCost(tabOrigin_.value(m).path, passRows_, passBytes_).over) {
            statusBar()->showMessage(
                tr("%1: the filter and sort were not applied again; on this file they read every row")
                    .arg(QString::fromStdString(m->tabLabel())), 8000);
            st.filter.clear();
            st.sort.clear();
        }
        if (!st.filter.isEmpty()) {
            FilterExpr fx;
            std::string err;
            if (parse_filter_expr(st.filter.toStdString(), *m->schema(), &fx, &err)) {
                filterText_[m] = st.filter;
                m->setFilterAsync(fx);
                recompute = true;
                if (m == activeModel()) filterEdit_->setText(st.filter);
            }
        }
        std::vector<ArrowTableModel::SortKey> keys;
        for (const auto& [name, desc] : st.sort)
            if (int c = colOf(name); c >= 0) keys.push_back({c, desc ? Qt::DescendingOrder : Qt::AscendingOrder});
        if (!keys.empty()) { setSortKeys(m, v, std::move(keys), /*ask=*/false); recompute = true; }
        if (const int c = colOf(st.curColumn); c >= 0) {
            pendingCursor_[m] = {st.curRow, c};
            if (!recompute) placePendingCursor(m, v);
        }
        if (m == activeModel()) refreshColumnsMenu();
    }
    // The cursor a view state asked for, once the tab's rows are there.
    void placePendingCursor(ArrowTableModel* m, QTableView* v) {
        auto it = pendingCursor_.find(m);
        if (it == pendingCursor_.end() || m->isComputing() || m->rowCount() == 0) return;
        const auto [row, col] = *it;
        pendingCursor_.erase(it);
        keepCol_.remove(m);
        const QModelIndex idx = m->index((int)std::clamp<qint64>(row, 0, m->rowCount() - 1), col);
        v->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
        v->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    }

    // View ▸ Reload (F5): re-read the active tab's file with its options;
    // each tab keeps its filter, sort, columns and cursor.
    bool reloadFile(int id) {
        const OpenFile* f = fileById(id);
        if (!f || f->jsonTree) return false;
        QHash<QString, TabViewState> states;
        for (int i = 0; i < tabs_->count(); ++i)
            if (tabFile_.value(tabs_->widget(i), 0) == id) states.insert(tabs_->label(i), captureViewState(i));
        const FileOptions opts = f->opts;
        const bool ok = reopenFile(id, opts, states);
        if (ok) {
            stampFile(id);
            changedFiles_.remove(id);
            syncFileControls();
            statusBar()->showMessage(tr("Reloaded %1").arg(QFileInfo(fileById(id)->path).fileName()), 3000);
        }
        return ok;
    }

    // ── Files changed on disk ────────────────────────────────────────────────
    // Each local file's size and modification time when it was read; a file
    // watcher (and, since inotify misses changes made on another machine of
    // a network file system, a check whenever the window is activated)
    // compares them and offers Reload in the file's strip.
    struct FileStamp { qint64 size = -1; QDateTime mtime; };
    static FileStamp statFile(const QString& path) {
        const QFileInfo fi(path);
        FileStamp s;
        if (fi.exists()) { s.size = fi.isDir() ? 0 : fi.size(); s.mtime = fi.lastModified(); }
        return s;
    }
    void stampFile(int id) {
        const OpenFile* f = fileById(id);
        if (!f || is_remote_url(f->path.toStdString())) return;
        stamps_[id] = statFile(f->path);
        if (!watcher_) {
            watcher_ = new QFileSystemWatcher(this);
            watchTimer_ = new QTimer(this);
            watchTimer_->setSingleShot(true);
            watchTimer_->setInterval(500);   // a writer appends in bursts
            connect(watchTimer_, &QTimer::timeout, this, [this] { checkChangedFiles(); });
            auto kick = [this] { watchTimer_->start(); };
            connect(watcher_, &QFileSystemWatcher::fileChanged, this, kick);
            connect(watcher_, &QFileSystemWatcher::directoryChanged, this, kick);
        }
        if (!watcher_->files().contains(f->path) && !watcher_->directories().contains(f->path))
            watcher_->addPath(f->path);
    }
    // Compare every watched file with its stamp; a changed one gets the bar.
    void checkChangedFiles() {
        bool any = false;
        for (const OpenFile& f : files_) {
            auto it = stamps_.find(f.id);
            if (it == stamps_.end()) continue;
            const FileStamp now = statFile(f.path);
            // A file replaced by rename (editors, atomic writers) drops off the watcher.
            if (now.size >= 0 && watcher_ && !watcher_->files().contains(f.path) &&
                !watcher_->directories().contains(f.path))
                watcher_->addPath(f.path);
            if (now.size == it->size && now.mtime == it->mtime) continue;
            if (!changedFiles_.contains(f.id)) { changedFiles_.insert(f.id); any = true; }
        }
        if (any) syncFileControls();
    }
    // Ignore: no bar until the file changes again.
    void ignoreChange(int id) {
        if (const OpenFile* f = fileById(id)) stamps_[id] = statFile(f->path);
        changedFiles_.remove(id);
        syncFileControls();
    }

    // ── Session: the open files, their options and each tab's view ─────────
    // Saved when the window closes; restored when vvg starts with no files
    // (View ▸ Restore Session at Start).
    QJsonObject sessionJson() const {
        QJsonArray files;
        QHash<int, int> index;   // file id -> position in `files`
        QHash<int, QJsonArray> tabsOf;
        for (int i = 0; i < tabs_->count(); ++i) {
            const int id = tabFile_.value(tabs_->widget(i), 0);
            const OpenFile* f = id ? fileById(id) : nullptr;
            if (!f) continue;
            if (!index.contains(id)) {
                index.insert(id, files.size());
                files.append(QJsonObject{{QStringLiteral("path"), f->path},
                                         {QStringLiteral("json"), f->jsonTree},
                                         {QStringLiteral("options"), f->opts.toJson()}});
            }
            const TabViewState st = captureViewState(i);
            if (!st.isDefault()) tabsOf[id].append(st.toJson());
        }
        for (auto it = tabsOf.cbegin(); it != tabsOf.cend(); ++it) {
            QJsonObject f = files[index.value(it.key())].toObject();
            f[QStringLiteral("tabs")] = it.value();
            files[index.value(it.key())] = f;
        }
        QJsonObject out{{QStringLiteral("files"), files}};
        if (const OpenFile* f = activeFile()) {
            out[QStringLiteral("currentFile")] = index.value(f->id, 0);
            out[QStringLiteral("currentTab")] = tabs_->label(tabs_->currentIndex());
        }
        return out;
    }
    void saveSession() {
        saveSetting(QStringLiteral("session"),
                    QString::fromUtf8(QJsonDocument(sessionJson()).toJson(QJsonDocument::Compact)));
    }
public:
    // Reopen the saved session; files no longer there are named in the
    // status bar. Returns the number of files opened.
    int restoreSession() {
        const QJsonObject o = QJsonDocument::fromJson(
            QSettings(QStringLiteral("vv"), QStringLiteral("vvg")).value(QStringLiteral("session")).toString().toUtf8()).object();
        const QJsonArray files = o.value(QStringLiteral("files")).toArray();
        QStringList missing;
        QList<int> ids;   // per saved file, its id now (0: not reopened)
        int opened = 0;
        for (const QJsonValue& fv : files) {
            const QJsonObject f = fv.toObject();
            const QString path = f.value(QStringLiteral("path")).toString();
            if (!is_remote_url(path.toStdString()) && !QFileInfo::exists(path)) {
                missing << path;
                ids << 0;
                continue;
            }
            QHash<QString, TabViewState> states;
            for (const QJsonValue& tv : f.value(QStringLiteral("tabs")).toArray()) {
                const TabViewState st = TabViewState::fromJson(tv.toObject());
                states.insert(st.label, st);
            }
            const int before = nextFileId_;
            const FileOptions keep = newFileOptions_;
            newFileOptions_ = FileOptions::fromJson(f.value(QStringLiteral("options")).toObject());
            openPaths({path}, /*quiet=*/true);
            newFileOptions_ = keep;
            const int id = nextFileId_ > before && fileById(nextFileId_ - 1) ? nextFileId_ - 1 : 0;
            ids << id;
            if (!id) continue;
            ++opened;
            giveStates(id, states);
        }
        // The tab shown last.
        const int cf = o.value(QStringLiteral("currentFile")).toInt(-1);
        const QString ct = o.value(QStringLiteral("currentTab")).toString();
        if (cf >= 0 && cf < ids.size() && ids[cf])
            for (int i = 0; i < tabs_->count(); ++i)
                if (tabFile_.value(tabs_->widget(i), 0) == ids[cf] && tabs_->label(i) == ct) {
                    tabs_->setCurrentIndex(i);
                    break;
                }
        if (!missing.isEmpty())
            statusBar()->showMessage(tr("Not found, not reopened: %1").arg(missing.join(QStringLiteral(", "))), 15000);
        return opened;
    }
private:
    // Give file `id`'s tabs the view states of their labels: a built tab at
    // once, a tab not shown yet when it is built.
    void giveStates(int id, const QHash<QString, TabViewState>& states) {
        for (int i = 0; i < tabs_->count(); ++i) {
            QWidget* w = tabs_->widget(i);
            if (tabFile_.value(w, 0) != id) continue;
            auto st = states.find(tabs_->label(i));
            if (st == states.end()) continue;
            if (pending_.count(w)) pendingState_[w] = *st;
            else if (i < (int)models_.size() && models_[(size_t)i])
                applyViewState(models_[(size_t)i], views_[(size_t)i], *st);
        }
    }

    // ── Reopen Closed Tab (Ctrl+Shift+T) ─────────────────────────────────────
    struct ClosedTab {
        QString path; FileOptions opts; bool jsonTree = false; TabViewState state;
        int offset = 0;   // its place among its file's tabs
    };
    void rememberClosedTab(int i) {
        const int id = tabFile_.value(tabs_->widget(i), 0);
        const OpenFile* f = id ? fileById(id) : nullptr;
        if (!f) return;
        int offset = 0;
        for (int j = 0; j < i; ++j) offset += tabFile_.value(tabs_->widget(j), 0) == id;
        closedTabs_.append(ClosedTab{f->path, f->opts, f->jsonTree, captureViewState(i), offset});
        while (closedTabs_.size() > 20) closedTabs_.removeFirst();
        if (reopenClosedAct_) reopenClosedAct_->setEnabled(true);
    }
public:
    // Open the last closed tab again: its file's tab of that label only, next
    // to the file's other tabs when the file is still open, with its view.
    bool reopenClosedTab() {
        if (closedTabs_.isEmpty()) return false;
        const ClosedTab c = closedTabs_.takeLast();
        if (reopenClosedAct_) reopenClosedAct_->setEnabled(!closedTabs_.isEmpty());
        if (c.jsonTree) { openPaths({c.path}); return true; }
        LoadResult r = loadFile(c.path, c.opts);
        int k = -1;
        for (size_t i = 0; i < r.sources.size() && k < 0; ++i)
            if (QString::fromStdString(r.sources[i]->tab_label()) == c.state.label) k = (int)i;
        if (k < 0) {   // the file no longer has that tab: open it whole
            const FileOptions keep = newFileOptions_;
            newFileOptions_ = c.opts;
            openPaths({c.path});
            newFileOptions_ = keep;
            return !r.sources.empty();
        }
        // The other tabs' sources go on their owner thread, as a closed
        // placeholder's do.
        for (size_t i = 0; i < r.sources.size(); ++i) {
            if ((int)i == k) continue;
            std::shared_ptr<TabularSource> dead = std::move(r.sources[i]);
            r.owners[i]->post([dead]() mutable { dead.reset(); });
        }
        int id = 0;
        for (const OpenFile& f : files_)
            if (f.path == c.path && f.opts == c.opts && !f.jsonTree) { id = f.id; break; }
        int at = tabs_->count();
        if (id) {
            // Back at its place among the file's tabs (after the last when
            // fewer are open now).
            int seen = 0;
            for (int i = 0; i < tabs_->count(); ++i) {
                if (tabFile_.value(tabs_->widget(i), 0) != id) continue;
                if (seen++ == c.offset) { at = i; break; }
                at = i + 1;
            }
        } else {
            OpenFile f{nextFileId_++, c.path, c.opts, false};
            f.feat = file_features(c.path.toStdString());
            files_.push_back(f);
            id = f.id;
            stampFile(id);
        }
        addSourceTab(std::move(r.sources[(size_t)k]), r.origins[(size_t)k], r.primary[(size_t)k],
                     r.expands[(size_t)k], r.owners[(size_t)k], id, /*now=*/false, at);
        pendingState_[tabs_->widget(at)] = c.state;
        tabs_->setCurrentIndex(at);
        relabelTabs();
        updateTitle();
        return true;
    }
private:

    // `states`: view states to give the new tabs, by label (Reload).
    bool reopenFile(int id, const FileOptions& opts,
                    const QHash<QString, TabViewState>& states = {}) {
        OpenFile* f = fileById(id);
        if (!f || f->jsonTree) return false;
        LoadResult r = loadFile(f->path, opts);
        if (r.sources.empty()) {
            const QString why = r.errors.isEmpty() ? tr("no data for this query")
                                                   : r.errors.join(QStringLiteral("; "));
            if (headless_) std::fprintf(stderr, "vvg region: %s\n", why.toStdString().c_str());
            statusBar()->showMessage(tr("Not applied: %1").arg(why), 15000);
            syncFileControls();           // back to the options the tabs show
            if (contextBar_) contextBar_->showError(why, opts);
            return false;
        }
        f->opts = opts;
        QWidget* cur = tabs_->currentWidget();
        const bool curInFile = tabFile_.value(cur, 0) == id;
        const QString curLabel = curInFile ? tabs_->label(tabs_->currentIndex()) : QString();
        // The current tab's place among its file's tabs: the tab shown again
        // when no new tab has its label ("X (preview)" becomes "X (long)").
        int curOffset = 0;
        for (int i = 0; curInFile && i < tabs_->currentIndex(); ++i)
            curOffset += tabFile_.value(tabs_->widget(i), 0) == id;
        int at = -1, target = -1;
        {
            QSignalBlocker quiet(tabs_);   // don't build each next tab as the current one closes
            for (int i = tabs_->count() - 1; i >= 0; --i)
                if (tabFile_.value(tabs_->widget(i), 0) == id) { closeTab(i, /*keepFile=*/true); at = i; }
            if (at < 0) at = tabs_->count();
            for (size_t i = 0; i < r.sources.size(); ++i) {
                const int idx = at + (int)i;
                addSourceTab(std::move(r.sources[i]), r.origins[i], r.primary[i], r.expands[i],
                             r.owners[i], id, /*now=*/false, idx);
                if (auto st = states.find(tabs_->label(idx)); st != states.end())
                    pendingState_[tabs_->widget(idx)] = *st;
                if (curInFile && target < 0 && tabs_->label(idx) == curLabel) target = idx;
            }
            if (curInFile && target < 0) target = at + std::min(curOffset, (int)r.sources.size() - 1);
            if (!curInFile) target = tabs_->indexOf(cur);
            tabs_->setCurrentIndex(target);
        }
        relabelTabs();
        onCurrentTab(target);
        return true;
    }

    OpenFile* fileById(int id) {
        for (auto& f : files_) if (f.id == id) return &f;
        return nullptr;
    }
    const OpenFile* fileById(int id) const {
        for (const auto& f : files_) if (f.id == id) return &f;
        return nullptr;
    }
    const OpenFile* activeFile() const { return fileById(tabFile_.value(tabs_->currentWidget(), 0)); }

    static QString partialText(const ArrowTableModel* m) {
        return tr("The first %1 rows (%2 in memory); the file goes on")
            .arg(QLocale().toString((qlonglong)m->rowCount()))
            .arg(QLocale().formattedDataSize(m->loadedBytes(), 1, QLocale::DataSizeTraditionalFormat));
    }
    void loadMoreRows() {
        auto* m = activeModel();
        if (!m || !m->budgetReached()) return;
        m->loadMore();
        syncFileControls();
        refreshStatus();
    }

    // Show the active tab's format options under the tab bar: the ones its
    // file and tab offer, as the file was opened; none for a tab with no file
    // to re-open (a JSON tree, a table made from one).
    void syncFileControls() {
        if (!contextBar_) return;
        const OpenFile* f = activeFile();
        if (f && filesWithTabs() >= 2)
            contextBar_->setFile(QFileInfo(f->path).fileName(), f->path, fileColor(f->id));
        else
            contextBar_->setFile({}, {}, {});
        contextBar_->setChanged(f && changedFiles_.contains(f->id)
                                    ? QFileInfo(f->path).fileName() : QString());
        auto* am = activeModel();
        contextBar_->setPartial(am && am->budgetReached() ? partialText(am) : QString());
        if (loadMoreAct_) loadMoreAct_->setEnabled(am && am->budgetReached());
        if (!f || f->jsonTree) { contextBar_->clearState(); return; }
        auto* m = activeModel();
        contextBar_->setState(f->feat, m ? m->tabTraits() : TabTraits{}, f->opts);
    }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        QStringList paths;
        for (const QUrl& u : e->mimeData()->urls()) {
            if (u.isLocalFile()) paths << u.toLocalFile();
            else if (remote_supported(u.toString().toStdString())) paths << u.toString();
        }
        openPaths(paths);
    }
    void resizeEvent(QResizeEvent* e) override {
        QMainWindow::resizeEvent(e);
        compactToolbar();
    }
    void closeEvent(QCloseEvent* e) override {
        saveSession();
        saveSetting(QStringLiteral("geometry"), saveGeometry());
        saveSetting(QStringLiteral("windowState"), saveState());
        QMainWindow::closeEvent(e);
    }
    // Write one setting on the settings thread, in call order; an invalid
    // value removes the key.
    void saveSetting(const QString& key, const QVariant& value) {
        QMetaObject::invokeMethod(settingsCtx_, [key, value] {
            QSettings st(QStringLiteral("vv"), QStringLiteral("vvg"));
            if (value.isValid()) st.setValue(key, value);
            else st.remove(key);
        }, Qt::QueuedConnection);
    }
    // The header of `v` shows the type line as View ▸ Column Types says.
    void syncHeaderTypes(QTableView* v) {
        if (auto* th = v ? dynamic_cast<TypedHeaderView*>(v->horizontalHeader()) : nullptr)
            th->setShowTypes(showTypes_);
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::WindowActivate && !stamps_.isEmpty()) checkChangedFiles();
        return QMainWindow::event(e);
    }
    void changeEvent(QEvent* e) override {
        if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange ||
            e->type() == QEvent::FontChange || e->type() == QEvent::ApplicationFontChange ||
            e->type() == QEvent::StyleChange) {
            refreshIcons();
            QTimer::singleShot(0, this, [this] { sizeToolbarButtons(); });   // once the size hints follow
        }
        QMainWindow::changeEvent(e);
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
        colTimer_ = new QTimer(this);
        colTimer_->setSingleShot(true);
        colTimer_->setInterval(150);
        connect(colTimer_, &QTimer::timeout, this, [this] { refreshColumnStats(); });
        statsPoll_ = new QTimer(this);
        statsPoll_->setInterval(250);
        connect(statsPoll_, &QTimer::timeout, this, [this] { showColumnStatsProgress(); });
        statsWatcher_ = new QFutureWatcher<StatsResult>(this);
        connect(statsWatcher_, &QFutureWatcher<StatsResult>::finished, this,
                [this] { onColumnStatsDone(); });
        // The Details dock: the cursor row (Row tab) and the cursor column's
        // summary, computed on demand (Column tab).
        detailsDock_ = new QDockWidget(tr("Details"), this);
        detailsDock_->setObjectName(QStringLiteral("detailsDock"));
        detailsDock_->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
        detailTabs_ = new QTabWidget(detailsDock_);
        detailTabs_->setDocumentMode(true);
        detail_ = new QTableWidget(0, 2, detailTabs_);
        detail_->setHorizontalHeaderLabels({tr("Column"), tr("Value")});
        detail_->horizontalHeader()->setStretchLastSection(true);
        detail_->verticalHeader()->setVisible(false);
        detail_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        colPanel_ = new ColumnPanel(detailTabs_);
        detailTabs_->addTab(detail_, tr("Row"));
        detailTabs_->addTab(colPanel_, tr("Column"));
        detailsDock_->setWidget(detailTabs_);
        addDockWidget(Qt::RightDockWidgetArea, detailsDock_);
        // Column statistics run while the Column tab is on screen.
        auto columnTabShown = [this] {
            if (detailsDock_->isVisible() && detailTabs_->currentWidget() == colPanel_) colTimer_->start();
            else cancelColumnStats();
        };
        connect(detailsDock_, &QDockWidget::visibilityChanged, this, columnTabShown);
        connect(detailTabs_, &QTabWidget::currentChanged, this, columnTabShown);
        QAction* details = detailsDock_->toggleViewAction();
        details->setText(tr("&Details"));
        details->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
        details->setToolTip(tr("Show or hide the Details panel: the cursor row and the "
                               "cursor column's statistics (Ctrl+B)"));
        detailsAct_ = details;
        setActionIcon(details, "view-right-new", Glyph::Details);
        connect(colPanel_, &ColumnPanel::cancelRequested, this, [this] {
            cancelColumnStats();
            colPanel_->showNote(tr("Column"), tr("Canceled."));
        });
        connect(colPanel_, &ColumnPanel::computeRequested, this, [this] { computeColumnStatsRequested(); });
        connect(colPanel_, &ColumnPanel::filterToValueRequested, this,
                [this](int i) { filterToValue(i); });
        connect(colPanel_, &ColumnPanel::copyCommandRequested, this, [this] { copyColumnCommand(); });
        connect(colPanel_, &ColumnPanel::showColumnRequested, this, [this] { showCursorColumn(); });
    }

    // ── Column tab ──────────────────────────────────────────────────────────
    // Summaries come from summarize_columns over a second copy of the tab,
    // opened on a worker (open_view_source, as export does), so the table
    // stays usable while a large file is scanned. One scan covers every
    // column (up to kStatsAllColumns): on a row-oriented file one column
    // costs the whole read anyway, and moving the cursor is then instant.
    // A large table (statsOnRequest) is summarised only when the Compute
    // button is clicked, and only the cursor column. Results are cached per
    // tab and filter.
    struct StatsResult {
        std::string                error;
        std::vector<int>           cols;        // source columns, parallel to sums
        std::vector<ColumnSummary> sums;
        int64_t                    totalRows = -1;   // of the copy read
        PreviewLimit               limit;            // of the copy read
    };
    struct StatsCache {
        QString                    filter;
        std::map<int, ColumnSummary> bySrcCol;
        int64_t                    totalRows = -1;
        PreviewLimit               limit;
    };
    static constexpr int kStatsAllColumns = 256;
    // A larger table is summarised only on request, one column at a time:
    // a scan reads every row of the file (all of it, on a row-oriented
    // format), which on a 3.4 G-row table takes hours. "Larger": over the
    // limits a sort or filter asks at (ArrowTableModel::fullPassCost).
    int64_t statsAutoRows_ = ArrowTableModel::kDefaultPassRows;   // VVG_STATS_AUTO_ROWS (self-test)
    bool statsOnRequest(ArrowTableModel* m) const {
        return m->fullPassCost(tabOrigin_.value(m).path, statsAutoRows_, passBytes_).over;
    }

    // Before a sort, filter or find of `m` that reads every row of its file:
    // over the limits the user is asked first. `what` names the job
    // ("Sorting by QNAME"). False: the view stays as it is.
    bool confirmFullPass(ArrowTableModel* m, const QString& what) {
        const QString path = tabOrigin_.value(m).path;
        const auto c = m->fullPassCost(path, passRows_, passBytes_);
        if (!c.over) return true;
        ++passAsked_;
        if (passAnswer_ >= 0) return passAnswer_ == 1;
        const QString size = c.rows >= 0 ? tr("%L1 rows").arg(c.rows)
                           : c.remote    ? tr("a remote file")
                                         : QLocale().formattedDataSize(c.fileBytes, 1,
                                               QLocale::DataSizeTraditionalFormat);
        QMessageBox box(QMessageBox::Warning, tr("Read the Whole File?"),
                        tr("%1 reads all of %2 (%3) into memory.")
                            .arg(what, QFileInfo(path).fileName(), size),
                        QMessageBox::NoButton, this);
        box.setInformativeText(
            tr("On a file read front to back (BAM, CRAM, FASTQ, text, …) the rows it reads stay in "
               "memory until the tab is closed, and a large file can take minutes and more memory "
               "than the computer has. To look at a place in an indexed BAM, CRAM or VCF, a region "
               "is faster.\n\nvvg asks above %L1 rows or, while a file's row count is not known, "
               "above a %2 file (View \u25b8 Memory Limits…).")
                .arg(passRows_)
                .arg(QLocale().formattedDataSize(passBytes_, 1, QLocale::DataSizeTraditionalFormat)));
        QPushButton* go = box.addButton(tr("Continue"), QMessageBox::AcceptRole);
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        QPushButton* limits = box.addButton(tr("Change Limits…"), QMessageBox::ActionRole);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() == limits)
            return editMemoryLimits() && confirmFullPass(m, what);
        return box.clickedButton() == go;
    }

    // View ▸ Memory Limits…: the load budget and the full-pass limits.
    bool editMemoryLimits() {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Memory Limits"));
        auto* form = new QFormLayout;
        const qint64 MiB = 1 << 20;
        auto* load = new QSpinBox(&dlg);
        load->setRange(16, 1 << 24);
        load->setSingleStep(256);
        load->setSuffix(tr(" MiB"));
        load->setValue((int)std::min<qint64>(loadBudget_ / MiB, 1 << 24));
        load->setToolTip(tr("A file read front to back keeps the rows read as the table scrolls; "
                            "past this much vvg stops and offers Load More"));
        auto* rows = new QSpinBox(&dlg);
        rows->setRange(1000, std::numeric_limits<int>::max());
        rows->setSingleStep(1'000'000);
        rows->setGroupSeparatorShown(true);
        rows->setValue((int)std::min<qint64>(passRows_, std::numeric_limits<int>::max()));
        auto* bytes = new QSpinBox(&dlg);
        bytes->setRange(1, 1 << 24);
        bytes->setSingleStep(256);
        bytes->setSuffix(tr(" MiB"));
        bytes->setValue((int)std::min<qint64>(std::max<qint64>(passBytes_ / MiB, 1), 1 << 24));
        form->addRow(tr("Rows read as the table scrolls, up to:"), load);
        auto* note = new QLabel(tr("A sort, filter or find reads every row. Ask first above:"), &dlg);
        note->setWordWrap(true);
        form->addRow(note);
        form->addRow(tr("Rows:"), rows);
        form->addRow(tr("File size, while its rows are not counted:"), bytes);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        auto* lay = new QVBoxLayout(&dlg);
        lay->addLayout(form);
        lay->addWidget(buttons);
        if (dlg.exec() != QDialog::Accepted) return false;
        setMemoryLimits(load->value() * MiB, rows->value(), bytes->value() * MiB);
        return true;
    }
    void setMemoryLimits(qint64 load, qint64 rows, qint64 bytes) {
        loadBudget_ = load;
        passRows_ = statsAutoRows_ = rows;
        passBytes_ = bytes;
        saveSetting(QStringLiteral("limits/loadBytes"), load);
        saveSetting(QStringLiteral("limits/passRows"), rows);
        saveSetting(QStringLiteral("limits/passBytes"), bytes);
        for (auto* m : models_) if (m) m->setLoadBudget(std::max<qint64>(load, m->loadedBytes()));
        syncFileControls();
    }

    // The cursor column; while a job has the model blank, the column the
    // cursor will come back in.
    int currentColumn(QTableView* v) const {
        const QModelIndex cur = v->selectionModel() ? v->selectionModel()->currentIndex()
                                                    : QModelIndex();
        if (cur.isValid()) return cur.column();
        auto* m = qobject_cast<ArrowTableModel*>(v->model());
        return m ? keepCol_.value(m, 0) : 0;
    }
    // Tell the Column tab where its column is: its position, and whether it
    // is hidden or scrolled out of the view.
    void updateColumnLocation() {
        if (!colPanel_) return;
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v || m->displayColumnCount() == 0) { colPanel_->setLocation(-1, 0, false); return; }
        const int col = std::min(currentColumn(v), m->displayColumnCount() - 1);
        const bool hidden = v->isColumnHidden(col);
        const int x = v->columnViewportPosition(col);
        const bool out = hidden || x + v->columnWidth(col) <= 0 || x >= v->viewport()->width();
        colPanel_->setLocation(col, m->displayColumnCount(), out, hidden);
    }
    // Scroll the Column tab's column into view (keeping the rows in view)
    // and flash it.
    void showCursorColumn() {
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v || m->displayColumnCount() == 0) return;
        const int col = std::min(currentColumn(v), m->displayColumnCount() - 1);
        if (v->isColumnHidden(col)) return;
        QHeaderView* h = v->horizontalHeader();
        if (m->rowCount() > 0)
            v->scrollTo(m->index(std::max(0, v->rowAt(0)), col), QAbstractItemView::EnsureVisible);
        else   // no rows to scroll to: move the header's scroll bar
            v->horizontalScrollBar()->setValue(
                v->horizontalScrollMode() == QAbstractItemView::ScrollPerPixel
                    ? h->sectionPosition(col) : h->visualIndex(col));
        flashRect(v->viewport(), QRect(v->columnViewportPosition(col), 0,
                                       v->columnWidth(col), v->viewport()->height()));
        flashRect(h->viewport(), QRect(h->sectionViewportPosition(col), 0,
                                       h->sectionSize(col), h->height()));
        v->setFocus();
        updateColumnLocation();
    }
    QString activeFilterText(ArrowTableModel* m) const {
        return m->hasFilter() ? filterText_.value(m) : QString();
    }
    void cancelColumnStats() {
        if (statsProg_) statsProg_->cancel = true;
        statsProg_.reset();
        statsModel_ = nullptr;
        statsPoll_->stop();
    }

    // Show the cursor column's summary: from the cache, from the scan in
    // progress, or by starting one. `force` runs even with the dock hidden
    // (the window self-test).
    void refreshColumnStats(bool force = false) {
        if (!colPanel_ || (!force && !colPanel_->isVisible())) return;
        updateColumnLocation();
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v) {
            colPanel_->showNote(tr("Column"), activeJsonTab()
                ? tr("A JSON tree has no columns. Right-click a list of records and open it as a table to summarise it.")
                : QString());
            return;
        }
        if (m->displayColumnCount() == 0) { colPanel_->showNote(tr("Column"), tr("No columns.")); return; }
        const int col = std::min(currentColumn(v), m->displayColumnCount() - 1);
        const int sc = m->sourceColumn(col);
        const QString title = m->columnName(col);
        const QString filter = activeFilterText(m);
        if (m->isComputing()) {
            colPanel_->showNote(title, tr("Waiting for the table's filter or sort to finish…"));
            return;
        }
        auto it = statsCache_.find(m);
        if (it != statsCache_.end() && it->filter == filter) {
            auto s = it->bySrcCol.find(sc);
            if (s != it->bySrcCol.end()) {
                colPanel_->showSummary(s->second, m->summableDigits(col),
                                       m->schema()->field(sc)->type(),
                                       statsScope(m, *it, s->second));
                return;
            }
        }
        if (statsProg_ && statsModel_ == m && statsFilter_ == filter &&
            std::find(statsCols_.begin(), statsCols_.end(), sc) != statsCols_.end()) {
            showColumnStatsProgress();
            return;
        }
        if (!tabOrigin_.value(m).reopen) {
            cancelColumnStats();
            colPanel_->showNote(title,
                tr("This tab was read from a pipe or from a node of a JSON document, so it "
                   "cannot be read again in the background. Compute summarises it here; "
                   "the window waits until it is done."), true);
            return;
        }
        if (statsOnRequest(m)) {
            // A scan of another column, if one runs, goes on and fills the cache.
            const int64_t rows = m->totalRows();
            const QString size = rows >= 0
                ? tr("This table has %L1 rows.").arg(rows)
                : tr("This file is %1.").arg(QLocale().formattedDataSize(
                      QFileInfo(tabOrigin_.value(m).path).size()));
            colPanel_->showNote(title,
                size + QLatin1Char(' ') +
                tr("Statistics read every row of the column, so on a table this large "
                   "they are computed only when asked, one column at a time."),
                true, tr("Compute Column Statistics"));
            return;
        }
        cancelColumnStats();
        startColumnStats(m, col, filter);
    }

    // The Column tab's Compute button: a tab that can be read again is
    // summarised in the background, the cursor column only; any other tab
    // here, with the window waiting.
    void computeColumnStatsRequested() {
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v || m->isComputing() || m->displayColumnCount() == 0) return;
        if (!tabOrigin_.value(m).reopen) { computeColumnStatsHere(); return; }
        const int col = std::min(currentColumn(v), m->displayColumnCount() - 1);
        cancelColumnStats();
        startColumnStats(m, col, activeFilterText(m), /*cursorOnly=*/true);
    }

    void startColumnStats(ArrowTableModel* m, int col, const QString& filter,
                          bool cursorOnly = false) {
        VvCommandSpec spec;
        if (!specForModel(m, viewOf(m), &spec)) return;
        Config cfg = vvCommandConfig(spec);
        cfg.describe = true;          // an AnnData frame is read in full
        cfg.select_cols.clear();
        cfg.sort_col.clear();
        cfg.sort_then.clear();
        cfg.filter_expr.clear();      // applied while summarising
        std::vector<int> cols;
        if (!cursorOnly && m->displayColumnCount() <= kStatsAllColumns)
            for (int c = 0; c < m->displayColumnCount(); ++c) cols.push_back(m->sourceColumn(c));
        else
            cols.push_back(m->sourceColumn(col));
        statsFutures_.erase(std::remove_if(statsFutures_.begin(), statsFutures_.end(),
                                           [](const QFuture<StatsResult>& f) { return f.isFinished(); }),
                            statsFutures_.end());
        auto prog = std::make_shared<ExportProgress>();
        statsProg_   = prog;
        statsModel_  = m;
        statsFilter_ = filter;
        statsCols_   = cols;
        auto schema = m->schema();
        const std::string ftext = filter.toStdString();
        QFuture<StatsResult> f = QtConcurrent::run([cfg, cols, schema, ftext, prog] {
            StatsResult r;
            r.cols = cols;
            std::unique_ptr<TabularSource> src;
            r.error = open_view_source(cfg, &src);
            if (!r.error.empty()) return r;
            if (!src->schema()->Equals(*schema)) {
                r.error = "the file has changed since it was opened; reopen it";
                return r;
            }
            FilterExpr fx;
            if (!ftext.empty() && !parse_filter_expr(ftext, *src->schema(), &fx, &r.error))
                return r;
            SummaryOptions opt;
            opt.cols         = cols;
            opt.filter       = ftext.empty() ? nullptr : &fx;
            opt.distinct_cap = 10000;
            opt.value_counts = true;
            opt.hist_bins    = 32;        // the Column tab's histogram
            opt.cancel       = &prog->cancel;
            opt.rows_done    = &prog->rows;
            r.error = summarize_columns(*src, opt, &r.sums);
            r.totalRows = src->total_rows();
            r.limit     = src->preview_limit();
            return r;
        });
        statsFutures_.append(f);
        statsWatcher_->setFuture(f);
        statsPoll_->start();
        showColumnStatsProgress();
    }

    void showColumnStatsProgress() {
        auto* m = activeModel();
        if (!statsProg_ || !m || statsModel_ != m) return;
        const PreviewLimit lim = m->previewLimit();
        const qint64 total = lim.rows_capped() ? lim.full_rows : m->totalRows();
        auto* v = activeView();
        const int col = v ? std::min(currentColumn(v), m->displayColumnCount() - 1) : 0;
        colPanel_->showBusy(m->columnName(col),
                            statsCols_.size() > 1 ? tr("Summarising every column… %1 rows read")
                                                  : tr("Summarising… %1 rows read"),
                            statsProg_->rows.load(), total);
    }

    void onColumnStatsDone() {
        // A superseded or canceled scan: its result is dropped.
        if (!statsProg_ || statsProg_->cancel) return;
        StatsResult r = statsWatcher_->result();
        ArrowTableModel* m = statsModel_;
        const QString filter = statsFilter_;
        statsProg_.reset();
        statsModel_ = nullptr;
        statsPoll_->stop();
        statsFutures_.erase(std::remove_if(statsFutures_.begin(), statsFutures_.end(),
                                           [](const QFuture<StatsResult>& f) { return f.isFinished(); }),
                            statsFutures_.end());
        if (!m || std::find(models_.begin(), models_.end(), m) == models_.end()) return;
        if (!r.error.empty()) {
            if (m == activeModel())
                colPanel_->showNote(tr("Column"), tr("Cannot summarise this tab: %1")
                                                      .arg(QString::fromStdString(r.error)));
            return;
        }
        StatsCache& c = statsCache_[m];
        if (c.filter != filter) c = StatsCache{};
        c.filter    = filter;
        c.totalRows = r.totalRows;
        c.limit     = r.limit;
        for (size_t i = 0; i < r.sums.size(); ++i) c.bySrcCol[r.cols[i]] = std::move(r.sums[i]);
        if (m == activeModel()) refreshColumnStats(true);
    }

    // A tab that cannot be re-read (stdin, a JSON node): summarise the
    // table's own source here, with the window waiting.
    void computeColumnStatsHere() {
        auto* m = activeModel();
        if (!m || m->isComputing()) return;
        std::vector<int> cols;
        for (int c = 0; c < m->displayColumnCount(); ++c) cols.push_back(c);
        std::vector<ColumnSummary> sums;
        QString err;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = m->summarize(cols, &sums, &err);
        QApplication::restoreOverrideCursor();
        if (!ok) { colPanel_->showNote(tr("Column"), tr("Cannot summarise this tab: %1").arg(err)); return; }
        StatsCache& c = statsCache_[m];
        c = StatsCache{};
        c.filter    = activeFilterText(m);
        c.totalRows = m->totalRows();
        c.limit     = m->previewLimit();
        for (size_t i = 0; i < sums.size(); ++i) c.bySrcCol[m->sourceColumn(cols[i])] = std::move(sums[i]);
        refreshColumnStats(true);
    }

    QString statsScope(ArrowTableModel* m, const StatsCache& c, const ColumnSummary& s) const {
        const qint64 rows = s.count + s.nulls;
        QString scope = c.filter.isEmpty()
            ? tr("All %L1 rows").arg(rows)
            : (c.totalRows >= 0 ? tr("Filter: %1 — %L2 of %L3 rows").arg(c.filter).arg(rows).arg(c.totalRows)
                                : tr("Filter: %1 — %L2 rows").arg(c.filter).arg(rows));
        if (c.limit.capped())
            scope += tr(". A preview: the first %L1 rows × %L2 columns of %L3 × %L4.")
                         .arg(c.limit.shown_rows).arg(c.limit.shown_cols)
                         .arg(c.limit.full_rows).arg(c.limit.full_cols);
        else if (m->previewLimit().rows_capped())
            scope += tr(" (the table shows the first %L1).").arg(m->previewLimit().shown_rows);
        return scope;
    }

    // Double-click on a value: narrow the filter to it.
    void filterToValue(int i) {
        auto* m = activeModel();
        const ColumnSummary* s = colPanel_->summary();
        if (!m || !s || i < 0 || i >= (int)s->values.size()) return;
        int sc = -1;
        for (int c = 0; c < m->displayColumnCount(); ++c)
            if (m->columnName(c).toStdString() == s->name) { sc = m->sourceColumn(c); break; }
        std::string expr, why;
        if (sc < 0 || !narrow_filter_to_value(activeFilterText(m).toStdString(), *m->schema(),
                                              sc, s->values[i], &expr, &why)) {
            statusBar()->showMessage(tr("filter: %1").arg(QString::fromStdString(why)), 4000);
            return;
        }
        filterEdit_->setText(QString::fromStdString(expr));
        applyFilter();
    }

    void copyColumnCommand() {
        auto* m = activeModel();
        auto* v = activeView();
        VvCommandSpec spec;
        if (!m || !v || !specForModel(m, v, &spec)) return;
        spec.select     = {m->columnName(std::min(currentColumn(v), m->displayColumnCount() - 1))};
        spec.sort.clear();
        spec.describe   = true;
        QApplication::clipboard()->setText(vvCommandLine(spec));
        statusBar()->showMessage(tr("Copied the vv command"), 2000);
    }

    void sortByColumn(ArrowTableModel* m, QTableView* v, int section, Qt::SortOrder ord) {
        setSortKeys(m, v, {{section, ord}});
    }
    void clearSortOf(ArrowTableModel* m, QTableView* v) {
        if (m->sortKeys().empty()) return;
        setSortKeys(m, v, {});
    }
    // Make `section` a further sort key with `ord` (moved to the end when it
    // is one already).
    void addSortKey(ArrowTableModel* m, QTableView* v, int section, Qt::SortOrder ord) {
        std::vector<ArrowTableModel::SortKey> keys;
        for (const auto& k : m->sortKeys()) if (k.col != section) keys.push_back(k);
        keys.push_back({section, ord});
        setSortKeys(m, v, std::move(keys));
    }
    // A header click: the column alone (ascending, descending, off), or with
    // Shift that column as a further key (added ascending; a key cycles to
    // descending, then is removed).
    void headerSortClick(ArrowTableModel* m, QTableView* v, int section, bool add) {
        std::vector<ArrowTableModel::SortKey> keys = m->sortKeys();
        auto it = std::find_if(keys.begin(), keys.end(),
                               [section](const auto& k) { return k.col == section; });
        if (!add) {
            if (keys.size() == 1 && it != keys.end()) {
                if (it->order == Qt::DescendingOrder) keys.clear();
                else it->order = Qt::DescendingOrder;
            } else {
                keys = {{section, Qt::AscendingOrder}};
            }
        } else if (it == keys.end()) {
            keys.push_back({section, Qt::AscendingOrder});
        } else if (it->order == Qt::AscendingOrder) {
            it->order = Qt::DescendingOrder;
        } else {
            keys.erase(it);
        }
        setSortKeys(m, v, std::move(keys));
    }
    // Sort `m` by `keys`; the header shows Qt's arrow for one key, and each
    // key's rank and direction (TypedHeaderView) for several.
    // `ask`: a sort the user started is confirmed when the file is large
    // (confirmFullPass); one given on the command line is not.
    void setSortKeys(ArrowTableModel* m, QTableView* v, std::vector<ArrowTableModel::SortKey> keys,
                     bool ask = true) {
        if (ask && !keys.empty()) {
            QStringList names;
            for (const auto& k : keys) names << m->columnName(k.col);
            if (!confirmFullPass(m, tr("Sorting by %1").arg(names.join(QStringLiteral(", "))))) return;
        }
        m->sortAsyncByKeys(keys);
        if (!v) return;
        QHeaderView* h = v->horizontalHeader();
        h->setSortIndicatorShown(keys.size() == 1);
        if (keys.size() == 1) h->setSortIndicator(keys.front().col, keys.front().order);
        h->viewport()->update();
    }
    QTableView* viewOf(ArrowTableModel* m) const {
        for (size_t i = 0; i < models_.size(); ++i)
            if (models_[i] == m) return views_[i];
        return nullptr;
    }

    // One toolbar row, as in gg: the Filter and Find boxes, then on the right
    // the Details panel toggle, Export… and the ≡ menu (also on a right-click
    // anywhere on the toolbar). Format options live in the Region bar below.
    void buildToolbar() {
        mainBar_ = addToolBar(tr("Main"));
        mainBar_->setObjectName(QStringLiteral("mainToolbar"));
        mainBar_->setMovable(false);
        mainBar_->setIconSize(QSize(22, 22));
        mainBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        mainBar_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(mainBar_, &QToolBar::customContextMenuRequested, this,
                [this](const QPoint& p) { appMenu_->popup(mainBar_->mapToGlobal(p)); });

        // Open (its ▾ lists the recent files), Open Directory, Copy, Copy Row.
        auto* openButton = new QToolButton(mainBar_);
        openButton->setDefaultAction(openAct_);
        openButton->setPopupMode(QToolButton::MenuButtonPopup);
        openButton->setMenu(recentMenu_);
        openButton->setAutoRaise(true);
        mainBar_->addWidget(openButton);
        mainBar_->addAction(openDirAct_);
        mainBar_->addSeparator();
        mainBar_->addAction(copyAct_);
        mainBar_->addAction(copyRowAct_);
        mainBar_->addSeparator();

        // The boxes in one widget, so the toolbar never squeezes them into
        // the buttons; capped so a wide window leaves the buttons at the right.
        searchGroup_ = new QWidget(mainBar_);
        auto* lay = new QHBoxLayout(searchGroup_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        filterEdit_ = new QLineEdit(searchGroup_);
        filterEdit_->setPlaceholderText(tr("Filter   e.g.  score > 5 and chrom == \"chr1\""));
        filterEdit_->setClearButtonEnabled(true);
        filterEdit_->setMinimumWidth(160);
        filterEdit_->setToolTip(tr("Keep the rows matching an expression (Enter applies; Ctrl+Shift+F)"));
        auto* filterIcon = filterEdit_->addAction(QIcon(), QLineEdit::LeadingPosition);
        setActionIcon(filterIcon, "view-filter", Glyph::Filter);
        connect(filterEdit_, &QLineEdit::returnPressed, this, [this]{ applyFilter(); });
        // Recent and saved filters of the tab's format family: the clock
        // menu, and completions while typing.
        auto* hist = filterEdit_->addAction(QIcon(), QLineEdit::TrailingPosition);
        setActionIcon(hist, "document-open-recent", Glyph::History);
        hist->setToolTip(tr("Recent and saved filters for this kind of file"));
        connect(hist, &QAction::triggered, this, [this] { filterHistoryMenu(); });
        filterHistModel_ = new QStringListModel(this);
        auto* completer = new QCompleter(filterHistModel_, filterEdit_);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        filterEdit_->setCompleter(completer);
        findEdit_ = new QLineEdit(searchGroup_);
        findEdit_->setPlaceholderText(tr("Find (regex) — Enter: next match"));
        findEdit_->setClearButtonEnabled(true);
        findEdit_->setMinimumWidth(120);
        findEdit_->setToolTip(tr("Highlight the cells matching a regular expression (Enter: next "
                                 "match, F3; Ctrl+F)"));
        auto* findIcon = findEdit_->addAction(QIcon(), QLineEdit::LeadingPosition);
        setActionIcon(findIcon, "edit-find", Glyph::Find);
        connect(findEdit_, &QLineEdit::returnPressed, this, [this]{ doFind(true); });
        connect(findEdit_, &QLineEdit::textChanged, this, [this](const QString& t){
            if (JsonTab* jt = activeJsonTab()) {
                jt->model->setSearch(t);
                jt->view->viewport()->update();
                return;
            }
            if (auto* m = activeModel()) {
                if (t.isEmpty()) m->clearSearch();
                else m->setSearch(QRegularExpression(
                         t, QRegularExpression::CaseInsensitiveOption));
            }
        });
        lay->addWidget(filterEdit_, 3);
        lay->addWidget(findEdit_, 2);
        searchGroup_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        searchGroup_->setMaximumWidth(900);
        mainBar_->addWidget(searchGroup_);

        auto* spacer = new QWidget(mainBar_);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        mainBar_->addWidget(spacer);

        detailsButton_ = new QToolButton(mainBar_);
        detailsButton_->setDefaultAction(detailsAct_);
        detailsButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        detailsButton_->setAutoRaise(true);
        mainBar_->addWidget(detailsButton_);

        exportButton_ = new QToolButton(mainBar_);
        exportButton_->setDefaultAction(exportAction_);
        exportButton_->setPopupMode(QToolButton::MenuButtonPopup);
        exportButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        exportButton_->setAutoRaise(true);
        exportButton_->setMenu(exportMenu_);
        mainBar_->addWidget(exportButton_);
        mainBar_->addSeparator();

        appButton_ = new QToolButton(mainBar_);
        appButton_->setDefaultAction(showAppMenuAct_);
        appButton_->setPopupMode(QToolButton::InstantPopup);
        appButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        appButton_->setAutoRaise(true);
        appButton_->setMenu(appMenu_);
        mainBar_->addWidget(appButton_);
        if (menuBar()->isNativeMenuBar()) appButton_->hide();
        sizeToolbarButtons();
    }

    // One height for every toolbar button (the tallest one's), so labelled
    // and icon-only buttons line up; re-run when fonts or the style change.
    void sizeToolbarButtons() {
        if (!mainBar_) return;
        QList<QToolButton*> buttons;
        for (QToolButton* b : mainBar_->findChildren<QToolButton*>())
            if (b->objectName() != QLatin1String("qt_toolbar_ext_button") && !searchGroup_->isAncestorOf(b))
                buttons << b;
        int h = 0;
        for (QToolButton* b : buttons) { b->setMinimumSize(0, 0); h = std::max(h, b->sizeHint().height()); }
        for (QToolButton* b : buttons) b->setMinimumHeight(h);
    }

    // The toolbar by window width: below 1150 px the Details and Export
    // buttons drop their labels (50 px of hysteresis, so a resize does not
    // flicker); the boxes shrink to their minimum widths before anything else.
    void compactToolbar(bool force = false) {
        const int w = width();
        int level = compactLevel_;
        if (level == 0 && w < 1150) level = 1;
        if (level == 1 && w >= 1200) level = 0;
        if (level == compactLevel_ && !force) return;
        compactLevel_ = level;
        const auto style = level >= 1 ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon;
        for (QToolButton* b : {detailsButton_, exportButton_}) if (b) b->setToolButtonStyle(style);
    }

    // The Details dock, on its Column tab.
    void showColumnTab() {
        detailsDock_->show();
        detailsDock_->raise();
        detailTabs_->setCurrentWidget(colPanel_);
    }

    void applyFilter(bool ask = true) {
        auto* m = activeModel();
        if (!m) return;
        QString text = filterEdit_->text().trimmed();
        if (text.isEmpty()) {
            filterEdit_->setStyleSheet({});
            filterText_.remove(m);
            m->clearFilterAsync();   // status updates on recomputeFinished
            return;
        }
        FilterExpr fx;
        std::string err;
        if (!parse_filter_expr(text.toStdString(), *m->schema(),
                               &fx, &err)) {
            filterEdit_->setStyleSheet("background:#ffd6d6;");
            statusBar()->showMessage(tr("filter: %1").arg(QString::fromStdString(err)));
            return;
        }
        filterEdit_->setStyleSheet({});
        if (ask && !confirmFullPass(m, tr("Filtering"))) {
            statusBar()->showMessage(tr("The filter was not applied"), 4000);
            return;
        }
        filterText_[m] = text;
        m->setFilterAsync(fx);       // runs off the UI thread; result on finish
        rememberFilter(text);
    }

    // The settings key of the active tab's format family ("alignments",
    // "hdf5", …; "other" for a file of no known family).
    QString filterFamilyKey() const {
        const OpenFile* f = activeFile();
        QString k = f ? QString::fromStdString(f->feat.family).section(QLatin1Char(' '), 0, 0).toLower()
                      : QString();
        k.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
        return k.isEmpty() ? QStringLiteral("other") : k;
    }
    // An applied filter heads the family's recent list (15 kept).
    void rememberFilter(const QString& text) {
        const QString key = filterFamilyKey();
        QStringList& r = recentFilters_[key];
        r.removeAll(text);
        r.prepend(text);
        while (r.size() > 15) r.removeLast();
        saveSetting(QStringLiteral("filters/recent/") + key, r);
        syncFilterCompletions();
    }
    void syncFilterCompletions() {
        if (filterHistModel_) filterHistModel_->setStringList(recentFilters_.value(filterFamilyKey()));
    }
    void saveFilter(const QString& name, const QString& expr) {
        const QString key = filterFamilyKey();
        savedFilters_[key][name] = expr;
        saveSetting(QStringLiteral("filters/saved/") + key, savedFilters_[key]);
    }
    void useFilter(const QString& expr) {
        filterEdit_->setText(expr);
        applyFilter();
    }
    // The clock menu of the Filter box: this family's recent and saved
    // filters, Save Current Filter…, Manage Saved Filters…, Clear Recent.
    void filterHistoryMenu() {
        const QString key = filterFamilyKey();
        const OpenFile* f = activeFile();
        const QString family = f && !f->feat.family.empty()
                                   ? QString::fromStdString(f->feat.family).section(QLatin1String(" ("), 0, 0)
                                   : tr("other files");
        QMenu menu(this);
        menu.setToolTipsVisible(true);
        auto item = [&](const QString& text, const QString& expr) {
            QAction* a = menu.addAction(fontMetrics().elidedText(text, Qt::ElideMiddle, 420));
            a->setToolTip(expr);
            connect(a, &QAction::triggered, this, [this, expr] { useFilter(expr); });
        };
        menu.addSection(tr("Recent: %1").arg(family));
        const QStringList recent = recentFilters_.value(key);
        for (const QString& e : recent) item(e, e);
        if (recent.isEmpty()) menu.addAction(tr("(none)"))->setEnabled(false);
        menu.addSection(tr("Saved: %1").arg(family));
        const QVariantMap saved = savedFilters_.value(key);
        for (auto it = saved.cbegin(); it != saved.cend(); ++it)
            item(it.key() + QStringLiteral("  \u2014  ") + it.value().toString(), it.value().toString());
        if (saved.isEmpty()) menu.addAction(tr("(none)"))->setEnabled(false);
        menu.addSeparator();
        QAction* save = menu.addAction(tr("Save Current Filter…"));
        save->setEnabled(!filterEdit_->text().trimmed().isEmpty());
        connect(save, &QAction::triggered, this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Save Filter"), tr("Name:"),
                                                       QLineEdit::Normal, {}, &ok).trimmed();
            if (ok && !name.isEmpty()) saveFilter(name, filterEdit_->text().trimmed());
        });
        QAction* manage = menu.addAction(tr("Manage Saved Filters…"));
        manage->setEnabled(!saved.isEmpty());
        connect(manage, &QAction::triggered, this, [this] { manageSavedFilters(); });
        QAction* clear = menu.addAction(tr("Clear Recent"));
        clear->setEnabled(!recent.isEmpty());
        connect(clear, &QAction::triggered, this, [this, key] {
            recentFilters_.remove(key);
            saveSetting(QStringLiteral("filters/recent/") + key, {});
            syncFilterCompletions();
        });
        menu.exec(filterEdit_->mapToGlobal(QPoint(filterEdit_->width() - menu.sizeHint().width(),
                                                  filterEdit_->height())));
    }
    // Rename or delete this family's saved filters.
    void manageSavedFilters() {
        const QString key = filterFamilyKey();
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Saved Filters"));
        dlg.resize(560, 360);
        auto* lay = new QVBoxLayout(&dlg);
        auto* list = new QListWidget(&dlg);
        auto fill = [&] {
            list->clear();
            const QVariantMap saved = savedFilters_.value(key);
            for (auto it = saved.cbegin(); it != saved.cend(); ++it) {
                auto* li = new QListWidgetItem(it.key() + QStringLiteral("  \u2014  ") + it.value().toString(), list);
                li->setData(Qt::UserRole, it.key());
            }
        };
        fill();
        lay->addWidget(list, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
        QPushButton* rename = buttons->addButton(tr("Rename…"), QDialogButtonBox::ActionRole);
        QPushButton* del = buttons->addButton(tr("Delete"), QDialogButtonBox::ActionRole);
        lay->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        connect(rename, &QPushButton::clicked, &dlg, [&] {
            QListWidgetItem* li = list->currentItem();
            if (!li) return;
            const QString old = li->data(Qt::UserRole).toString();
            bool ok = false;
            const QString name = QInputDialog::getText(&dlg, tr("Rename Filter"), tr("Name:"),
                                                       QLineEdit::Normal, old, &ok).trimmed();
            if (!ok || name.isEmpty() || name == old) return;
            QVariantMap& m = savedFilters_[key];
            m[name] = m.take(old);
            saveSetting(QStringLiteral("filters/saved/") + key, m);
            fill();
        });
        connect(del, &QPushButton::clicked, &dlg, [&] {
            QListWidgetItem* li = list->currentItem();
            if (!li) return;
            QVariantMap& m = savedFilters_[key];
            m.remove(li->data(Qt::UserRole).toString());
            saveSetting(QStringLiteral("filters/saved/") + key, m.isEmpty() ? QVariant() : QVariant(m));
            fill();
        });
        dlg.exec();
    }

    // Find in a JSON tree tab: the next / previous key or value matching the
    // query after the selected row, scanned off the UI thread (Cancel stops
    // it); the hit is opened and selected.
    void doJsonFind(JsonTab* jt, bool forward) {
        if (jt->searching()) return;
        const QString q = findEdit_->text();
        jt->model->setSearch(q);
        jt->view->viewport()->update();
        if (q.isEmpty()) return;
        const QModelIndex cur = jt->view->currentIndex();
        const vvjson::JNode n = jt->model->node(cur.isValid() ? cur : jt->model->index(0, 0));
        const uint64_t from = vvjson::JsonDoc::search_from(n, false);
        const uint64_t before = n.virt ? 0 : n.start();
        const vvjson::JsonDoc* doc = &jt->model->doc();
        const vvjson::JsonSearch pat = jt->model->search();
        if (!jt->finder) {
            jt->finder = new QFutureWatcher<uint64_t>(jt->view);
            connect(jt->finder, &QFutureWatcher<uint64_t>::finished, this,
                    [this, jt] { onJsonFindDone(jt); });
        }
        jt->findCancel = false;
        jt->findPos = 0;
        progress_->setVisible(true);
        cancelBtn_->setVisible(true);
        statusBar()->showMessage(tr("Searching…"));
        jt->finder->setFuture(QtConcurrent::run([jt, doc, pat, forward, from, before]() {
            auto tick = [jt](uint64_t pos) { jt->findPos = pos; return !jt->findCancel.load(); };
            return doc->search(pat, forward, from, before, tick, &jt->findWrapped, &jt->findCancelled);
        }));
    }
    void onJsonFindDone(JsonTab* jt) {
        progress_->setVisible(false);
        cancelBtn_->setVisible(false);
        const uint64_t hit = jt->finder->result();
        if (jt->findCancelled) { statusBar()->showMessage(tr("find: cancelled"), 2000); return; }
        if (hit == UINT64_MAX) { statusBar()->showMessage(tr("find: no match"), 2000); return; }
        const QModelIndex at = jt->model->indexForChain(jt->model->doc().path_to(hit));
        if (!at.isValid()) { statusBar()->showMessage(tr("find: no match"), 2000); return; }
        for (QModelIndex p = at.parent(); p.isValid(); p = p.parent()) jt->view->expand(p);
        jt->view->setCurrentIndex(at);
        jt->view->scrollTo(at, QAbstractItemView::PositionAtCenter);
        refreshStatus();
        if (jt->findWrapped)
            statusBar()->showMessage(statusBar()->currentMessage() + tr("  |  find wrapped"));
    }

    void doFind(bool forward) {
        if (JsonTab* jt = activeJsonTab()) { doJsonFind(jt, forward); return; }
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v || m->isComputing()) return;   // a worker already owns src_
        QString pat = findEdit_->text();
        if (pat.isEmpty()) { m->clearSearch(); return; }
        QRegularExpression re(pat, QRegularExpression::CaseInsensitiveOption);
        m->setSearch(re);                       // live cell highlight
        if (m->findResultsValid(re)) {
            jumpToFind(m, v, v->currentIndex(), forward);   // instant (F3 replay)
            return;
        }
        // Stale/new pattern: scan off the UI thread, jump when findReady fires.
        QModelIndex cur = v->currentIndex();
        pendingFindRow_     = cur.isValid() ? cur.row() : -1;
        pendingFindCol_     = cur.isValid() ? cur.column() : -1;
        pendingFindForward_ = forward;
        pendingFindModel_   = m;
        if (!confirmFullPass(m, tr("Finding"))) return;
        m->findAsync(re);
    }

    // Jump the view to the next/previous match relative to `from`.
    void jumpToFind(ArrowTableModel* m, QTableView* v,
                    const QModelIndex& from, bool forward) {
        QModelIndex hit = m->findNext(from, forward);
        if (hit.isValid()) {
            v->setCurrentIndex(hit);
            v->scrollTo(hit, QAbstractItemView::PositionAtCenter);
        } else {
            statusBar()->showMessage(tr("find: no match"), 2000);
        }
    }

    void onFindReady(ArrowTableModel* m, qint64 matches) {
        if (m == computingModel_) computingModel_ = nullptr;
        if (progress_)  progress_->setVisible(false);
        if (cancelBtn_) cancelBtn_->setVisible(false);
        if (m != activeModel() || m != pendingFindModel_) return;
        pendingFindModel_ = nullptr;
        if (matches == 0) {
            statusBar()->showMessage(tr("find: no match"), 2000);
            return;
        }
        auto* v = activeView();
        if (!v) return;
        QModelIndex from = (pendingFindRow_ >= 0 && pendingFindCol_ >= 0)
            ? m->index(pendingFindRow_, pendingFindCol_)
            : QModelIndex();   // no prior selection → search from the start
        statusBar()->showMessage(tr("find: %1 match(es)").arg(matches), 3000);
        jumpToFind(m, v, from, pendingFindForward_);
    }

    void stepSlice(int delta) {
        if (auto* m = activeModel()) {
            if (m->stepSlice(delta)) refreshStatus();
        }
    }

    // Show row `cur` in the detail dock. Moving the cursor along the same row
    // changes nothing, so it returns early; a new row rewrites the values in
    // the existing items, and the name column is rewritten (and resized) only
    // when the model or its column count changes.
    // The detail dock for a JSON tree tab: the selected node's path, type,
    // size and value (a string decoded, at most 64 KiB).
    void updateJsonDetail(JsonTab* jt, const QModelIndex& cur) {
        detailModel_ = nullptr; detailRow_ = -1;
        if (!cur.isValid()) { detail_->setRowCount(0); return; }
        const vvjson::JNode n = jt->model->node(cur);
        const QString type = jt->model->index(cur.row(), JsonTreeModel::Type, cur.parent()).data().toString();
        QStringList rows = {tr("Path"), jt->model->path(cur), tr("Type"), type};
        if (n.container())
            rows << tr("Children") << QString::number((qlonglong)n.count);
        if (!n.virt)
            rows << tr("Size") << QStringLiteral("%1 B").arg((qulonglong)(n.end - n.off));
        if (!n.container() && n.kind != vvjson::JKind::Error)
            rows << tr("Value") << jt->model->valueText(cur, 64 << 10);
        detail_->setRowCount(rows.size() / 2);
        for (int r = 0; r < rows.size() / 2; ++r)
            for (int c = 0; c < 2; ++c) {
                if (QTableWidgetItem* it = detail_->item(r, c)) it->setText(rows[2 * r + c]);
                else detail_->setItem(r, c, new QTableWidgetItem(rows[2 * r + c]));
            }
        detail_->resizeColumnToContents(0);
    }

    void updateDetail(const QModelIndex& cur) {
        if (!detail_) return;
        if (JsonTab* jt = activeJsonTab()) { updateJsonDetail(jt, jt->view->currentIndex()); return; }
        auto* m = activeModel();
        if (!m || !cur.isValid()) {
            detail_->setRowCount(0);
            detailModel_ = nullptr; detailRow_ = -1;
            return;
        }
        const int cols = m->displayColumnCount();
        const bool relabel = m != detailModel_ || detail_->rowCount() != cols;
        if (!relabel && cur.row() == detailRow_) return;
        detail_->setRowCount(cols);
        auto put = [&](int r, int c, const QString& text) {
            if (QTableWidgetItem* it = detail_->item(r, c)) it->setText(text);
            else detail_->setItem(r, c, new QTableWidgetItem(text));
        };
        for (int c = 0; c < cols; ++c) {
            if (relabel) put(c, 0, m->columnName(c));
            put(c, 1, m->data(m->index(cur.row(), c), Qt::DisplayRole).toString());
        }
        if (relabel) detail_->resizeColumnToContents(0);
        detailModel_ = m; detailRow_ = cur.row();
    }

    void copySelection() {
        if (colPanel_ && colPanel_->isAncestorOf(QApplication::focusWidget())) {
            if (colPanel_->copySelection())
                statusBar()->showMessage(tr("Copied"), 2000);
            return;
        }
        copySelectionAs(CopyFormat::Tsv, false);
    }

    // Copy the full row(s) touched by the selection (or the current cell) as
    // one TSV line each — every visible column, in on-screen order, raw values.
    void copyRows() { copySelectionAs(CopyFormat::Tsv, true); }

    enum class CopyFormat { Tsv, TsvHeader, Csv, Markdown };
    // The selection as a grid: its rows (view order) and its columns (display
    // columns, on-screen order, hidden ones left out); with `wholeRows` every
    // visible column. A cell of the grid that is not selected is empty.
    struct SelectionGrid { std::vector<int> rows, cols; QSet<quint64> cells; bool all = false; };
    SelectionGrid selectionGrid(QTableView* v, ArrowTableModel* m, bool wholeRows) const {
        SelectionGrid g;
        const QModelIndexList sel = v->selectionModel()->selectedIndexes();
        std::set<int> rows, cols;
        for (const QModelIndex& i : sel) {
            if (v->isColumnHidden(i.column())) continue;
            rows.insert(i.row());
            cols.insert(i.column());
            g.cells.insert(((quint64)i.row() << 32) | (quint32)i.column());
        }
        if (rows.empty() && v->currentIndex().isValid()) {
            rows.insert(v->currentIndex().row());
            cols.insert(v->currentIndex().column());
            g.cells.insert(((quint64)v->currentIndex().row() << 32) | (quint32)v->currentIndex().column());
        }
        g.rows.assign(rows.begin(), rows.end());
        auto* h = v->horizontalHeader();
        for (int vis = 0; vis < m->columnCount(); ++vis) {
            const int c = h->logicalIndex(vis);
            if (v->isColumnHidden(c)) continue;
            if (wholeRows || cols.count(c)) g.cols.push_back(c);
        }
        g.all = wholeRows;
        return g;
    }
    // `g` as text: TSV (no header, what Ctrl+C copies), TSV with a header
    // row, CSV (RFC 4180 quoting) or a Markdown table (numbers right-aligned).
    static QString gridText(ArrowTableModel* m, const SelectionGrid& g, CopyFormat f) {
        auto csv = [](QString t) {
            if (t.contains(QLatin1Char(',')) || t.contains(QLatin1Char('"')) || t.contains(QLatin1Char('\n')) ||
                t.contains(QLatin1Char('\r')) || t != t.trimmed()) {
                t.replace(QLatin1String("\""), QLatin1String("\"\""));
                t = QLatin1Char('"') + t + QLatin1Char('"');
            }
            return t;
        };
        auto md = [](QString t) {
            t.replace(QLatin1Char('|'), QLatin1String("\\|"));
            t.replace(QLatin1String("\r\n"), QLatin1String("<br>"));
            t.replace(QLatin1Char('\n'), QLatin1String("<br>"));
            return t;
        };
        auto cell = [&](const QString& t) {
            return f == CopyFormat::Csv ? csv(t) : f == CopyFormat::Markdown ? md(t) : t;
        };
        const QString sep = f == CopyFormat::Csv ? QStringLiteral(",")
                          : f == CopyFormat::Markdown ? QStringLiteral(" | ") : QStringLiteral("\t");
        auto line = [&](const QStringList& fields) {
            return f == CopyFormat::Markdown ? QStringLiteral("| ") + fields.join(sep) + QStringLiteral(" |")
                                             : fields.join(sep);
        };
        QStringList lines;
        if (f != CopyFormat::Tsv) {
            QStringList head, rule;
            for (int c : g.cols) {
                head << cell(m->columnName(c));
                const bool num = m->summableDigits(c) > 0;
                rule << (num ? QStringLiteral("---:") : QStringLiteral("---"));
            }
            lines << line(head);
            if (f == CopyFormat::Markdown) lines << line(rule);
        }
        ArrowTableModel::SyncReads now(m);   // rows not on screen are read here
        for (int r : g.rows) {
            QStringList fields;
            for (int c : g.cols) {
                const bool in = g.all || g.cells.contains(((quint64)r << 32) | (quint32)c);
                fields << (in ? cell(m->index(r, c).data(ArrowTableModel::RawTextRole).toString()) : QString());
            }
            lines << line(fields);
        }
        return lines.join(QLatin1Char('\n'));
    }
    void copySelectionAs(CopyFormat f, bool wholeRows) {
        auto* v = activeView();
        auto* m = activeModel();
        if (!v || !m) return;
        const SelectionGrid g = selectionGrid(v, m, wholeRows);
        if (g.rows.empty() || g.cols.empty()) return;
        QApplication::clipboard()->setText(gridText(m, g, f));
        statusBar()->showMessage(wholeRows ? tr("Copied %n row(s)", nullptr, (int)g.rows.size())
                                           : tr("Copied %1 × %2 cells").arg(g.rows.size()).arg(g.cols.size()),
                                 2000);
    }

    // The vv command line that reproduces the active tab (see vvcommand.h).
    // Returns "" when no tab is open.
public:
    QString commandForActiveTab() const {
        VvCommandSpec s;
        return specForActiveTab(&s) ? vvCommandLine(s) : QString();
    }
    // The active tab's state as command options (file, tab, session region
    // options, filter, sort, visible columns). False when no tab is open.
    bool specForActiveTab(VvCommandSpec* out) const {
        return specForModel(activeModel(), activeView(), out);
    }
    bool specForModel(ArrowTableModel* m, QTableView* v, VvCommandSpec* out) const {
        if (!m || !v) return false;
        VvCommandSpec& s = *out;
        s = VvCommandSpec{};
        const TabOrigin o = tabOrigin_.value(m);
        s.path = o.path.contains(QLatin1String("://")) ? o.path : QFileInfo(o.path).absoluteFilePath();
        if (!o.primary) s.tab = QString::fromStdString(m->tabLabel());
        // The format options of the tab's file (none for a table made from a
        // JSON tree node).
        const OpenFile* f = fileById(tabFile_.value(v, 0));
        const FileOptions fo = f ? f->opts : FileOptions{};
        s.region  = QString::fromStdString(fo.region);
        s.ncbi    = fo.ncbi;
        s.slop    = (int)fo.slop;
        s.pileup  = fo.pileup;
        s.tags    = QString::fromStdString(fo.tags);
        s.gtStats = fo.gtStats;
        s.contigs = fo.contigs;
        s.expand  = o.expand;
        s.samples = QString::fromStdString(fo.samples);
        s.matrix  = QString::fromStdString(fo.matrix);
        if (m->hasFilter()) s.filter = filterText_.value(m);
        for (const auto& k : m->sortKeys())
            s.sort.append({m->columnName(k.col), k.order == Qt::DescendingOrder});
        // Hidden columns: name the visible ones. --select takes an exact name
        // as-is, but splits on commas and trims spaces, so such a name is
        // given by its 1-based source position ("N-N") instead.
        // In on-screen order, so a reordered view (--select Score,Chr) copies
        // as it shows.
        const int ncols = m->displayColumnCount();
        QStringList visible;
        bool anyHidden = false, reordered = false;
        auto* h = v->horizontalHeader();
        for (int vis = 0; vis < ncols; ++vis) {
            const int c = h->logicalIndex(vis);
            if (c != vis) reordered = true;
            if (v->isColumnHidden(c)) { anyHidden = true; continue; }
            visible << selectTerm(m, c);
        }
        if (anyHidden || reordered) s.select = visible;
        return true;
    }
    // Headless export (VVG_WINTEST + VVG_EXPORT): start the export of the
    // active tab and pump the event loop until it finishes; with
    // cancelAfterRows >= 0, cancel once more rows than that are written.
    // Returns "" or the error, and the rows written.
    QString exportActiveForTest(const QString& out, qint64* rows,
                                qint64 cancelAfterRows = -1) {
        VvCommandSpec s;
        if (!specForActiveTab(&s)) return QStringLiteral("no tab");
        ExportFormat f;
        if (!exportFormatForPath(out, &f)) return QStringLiteral("unknown extension");
        startExport(s, out, f);
        while (exportWatcher_) {
            if (cancelAfterRows >= 0 && exportProgress_ &&
                exportProgress_->rows.load() > cancelAfterRows)
                exportProgress_->cancel = true;          // as the Cancel button
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
        }
        *rows = lastExportRows_;
        return lastExportError_;
    }
    // Sort / hide on the active tab, as the header click and View > Columns
    // do (used by --sort and the VVG_WINTEST hooks).
    bool sortActiveByName(const QString& column, bool desc) {
        return sortActiveByNames({column + (desc ? QStringLiteral(":desc") : QStringLiteral(":asc"))}).isEmpty();
    }
    // Sort the active tab by "COL[:asc|:desc]" keys, the first deciding.
    // Returns the first unknown column, or "" when sorted.
    QString sortActiveByNames(const QStringList& specs) {
        auto* m = activeModel();
        if (!m) return specs.value(0);
        std::vector<ArrowTableModel::SortKey> keys;
        for (QString col : specs) {
            Qt::SortOrder ord = Qt::AscendingOrder;
            if (col.endsWith(QStringLiteral(":desc"))) { ord = Qt::DescendingOrder; col.chop(5); }
            else if (col.endsWith(QStringLiteral(":asc"))) col.chop(4);
            int found = -1;
            for (int c = 0; c < m->displayColumnCount() && found < 0; ++c)
                if (m->columnName(c) == col) found = c;
            if (found < 0) return col;
            keys.push_back({found, ord});
        }
        setSortKeys(m, viewOf(m), std::move(keys), /*ask=*/false);
        for (int g = 0; m->isComputing() && g < 2000000; ++g)
            QCoreApplication::processEvents();
        return {};
    }
    bool hideActiveColumnForTest(const QString& column) {
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v) return false;
        for (int c = 0; c < m->displayColumnCount(); ++c)
            if (m->columnName(c) == column) { v->setColumnHidden(c, true); return true; }
        return false;
    }
    void selectTabForTest(int i) { tabs_->setCurrentIndex(i); waitIdleForTest(); }
    // Window self-test of the asynchronous reads: for each "row:col" of `spec`,
    // ask the active tab for the cell (a chunk not loaded shows "…" and is
    // requested), wait, and print what it shows then. ticks= counts a 50-ms
    // UI timer meanwhile: the UI keeps running while chunks are read.
    QString cellsForTest(const QString& spec) {
        waitIdleForTest();
        auto* m = activeModel();
        if (!m) return QStringLiteral("no model");
        int ticks = 0;
        QTimer tick;
        connect(&tick, &QTimer::timeout, this, [&ticks] { ++ticks; });
        tick.start(50);
        QStringList out;
        for (const QString& rc : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QStringList p = rc.split(QLatin1Char(':'));
            if (p.size() != 2) continue;
            const QModelIndex idx = m->index(p[0].toInt(), p[1].toInt());
            const QString before = m->data(idx, Qt::DisplayRole).toString();
            waitIdleForTest();
            out << QStringLiteral("%1=%2%3").arg(rc, m->data(idx, Qt::DisplayRole).toString(),
                                                  before == QStringLiteral("\u2026") ? QString()
                                                                                       : QStringLiteral("(cached)"));
        }
        return out.join(QLatin1Char(' ')) + QStringLiteral(" ticks=%1").arg(ticks);
    }
    // Window self-test: show tab `i` and close it at once, while its schema
    // is still being read (with VVG_TEST_READ_DELAY_MS); the read finishes on
    // the owner thread and is not delivered.
    QString closeLoadingForTest(int i) {
        tabs_->setCurrentIndex(i);
        auto* m = activeModel();
        const bool loading = m && !m->isReady();
        closeTab(tabs_->currentIndex());
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 800) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return QStringLiteral("loading=%1 tabs=%2").arg(loading ? 1 : 0).arg(tabs_->count());
    }
    // The self-test reads values right after an action; with reads on the
    // owner thread, pump events until every built tab has its schema, no job
    // and no chunk in flight (the view's requests included), up to 30 s.
    void waitIdleForTest() {
        QElapsedTimer t;
        t.start();
        auto busy = [this] {
            for (auto* m : models_)
                if (m && (!m->isReady() || m->isComputing() || m->hasPendingReads())) return true;
            return false;
        };
        do QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        while (busy() && t.elapsed() < 30000);
    }
    int  builtTabsForTest() const {
        int n = 0;
        for (auto* m : models_) n += m != nullptr;
        return n;
    }
    int  tabCountForTest() const { return tabs_->count(); }
    void closeTabForTest(int i) { closeTab(i); waitIdleForTest(); }
    // Window self-test: shown at `w` px wide, do any two controls of the
    // toolbar or of the strip under the tab bar overlap, or leave the window?
    QString overlapsForTest(int w) {
        show();
        resize(w, 600);
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        int overlaps = 0;
        auto check = [&](QWidget* host) {
            QList<QRect> boxes;
            for (QWidget* c : host->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
                if (!c->isVisible() || c->objectName() == QLatin1String("qt_toolbar_ext_button")) continue;
                if (qobject_cast<QMenu*>(c)) continue;
                const QRect g = c->geometry();
                if (g.isEmpty()) continue;
                for (const QRect& b : boxes) overlaps += b.intersects(g);
                if (!host->rect().contains(g)) ++overlaps;
                boxes << g;
            }
        };
        check(mainBar_);
        if (contextBar_->isVisible()) check(contextBar_);
        return QStringLiteral("width=%1 overlaps=%2").arg(width()).arg(overlaps);
    }
    // Window self-test: the active tab's first column header — its lines,
    // height, name and type line.
    QString headerForTest() {
        waitIdleForTest();
        auto* v = activeView();
        auto* m = activeModel();
        auto* th = v ? dynamic_cast<TypedHeaderView*>(v->horizontalHeader()) : nullptr;
        if (!m || !th || m->columnCount() == 0) return QStringLiteral("no header");
        const QString type = m->headerData(0, Qt::Horizontal, ArrowTableModel::TypeRole).toString();
        return QStringLiteral("lines=%1 height=%2 name='%3' type='%4'")
            .arg(th->showTypes() && !type.isEmpty() ? 2 : 1).arg(th->sizeHint().height())
            .arg(m->headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(), type);
    }
    // Window self-test: shown with ~60 rows on screen, let the view read and
    // repaint for `ms`; then whether reads stopped, how many batches were
    // read in the last half of that time, and the visible rows still unread.
    QString settleForTest(int ms) {
        show();
        resize(1100, 1600);
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v) return QStringLiteral("no table");
        QElapsedTimer t;
        t.start();
        int64_t half = -1;
        while (t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (half < 0 && t.elapsed() >= ms / 2) half = m->readBatches();
        }
        int unread = 0;
        const int first = v->rowAt(0), last = v->rowAt(v->viewport()->height() - 1);
        for (int r = std::max(0, first); r <= (last < 0 ? m->rowCount() - 1 : last); ++r)
            unread += m->data(m->index(r, 0), Qt::DisplayRole).toString() == QStringLiteral("\u2026");
        return QStringLiteral("idle=%1 late_batches=%2 rows=%3 unread=%4")
            .arg(m->hasPendingReads() ? 0 : 1).arg(m->readBatches() - std::max<int64_t>(0, half))
            .arg((last < 0 ? m->rowCount() - 1 : last) - std::max(0, first) + 1).arg(unread);
    }
    QString contextForTest() { waitIdleForTest(); syncFileControls(); return contextBar_->describeForTest(); }

    // vv's view flags on vvg's command line (vvg --filter … --sort … file).
    struct ViewOptions {
        QString filter, select, tab, region, tags, samples, matrix;
        QStringList sort;   // one --sort per key, "COL[:asc|:desc]"
        bool ncbi = false, pileup = false, gtStats = false, contigs = false;
        int  slop = 0;
        bool session() const {
            return !region.isEmpty() || !tags.isEmpty() || pileup || gtStats || contigs || slop ||
                   !samples.isEmpty() || !matrix.isEmpty();
        }
    };
    // Apply them after the files open, in vv's order: region / session options
    // (re-open), --tab, --select (hide the rest, in the given order), --filter,
    // --sort. Returns a message per option that could not be applied.
    QStringList applyViewOptions(const ViewOptions& o) {
        QStringList errs;
        auto settle = [&](ArrowTableModel* m) {
            for (int g = 0; m && (m->isComputing() || !m->isReady()) && g < 2000000; ++g)
                QCoreApplication::processEvents();
        };
        if (o.session()) {
            // As in vv: every file named on the command line takes the flags.
            std::vector<int> ids;
            for (const auto& f : files_) if (!f.jsonTree) ids.push_back(f.id);
            for (int id : ids)
                if (const OpenFile* f = fileById(id))
                    reopenFile(id, withQuery(f->opts, o.region, o.ncbi, o.slop, o.pileup, o.tags,
                                             o.gtStats, o.contigs, o.samples, o.matrix));
        }
        if (!o.tab.isEmpty()) {
            int found = -1;
            for (int i = 0; i < tabs_->count() && found < 0; ++i)
                if (tabs_->label(i) == o.tab) found = i;
            bool num = false;
            const int idx = o.tab.toInt(&num);
            if (found < 0 && num && idx >= 0 && idx < tabs_->count()) found = idx;
            if (found >= 0) tabs_->setCurrentIndex(found);
            else errs << tr("--tab: no tab named '%1'").arg(o.tab);
        }
        auto* m = activeModel();
        auto* v = activeView();
        settle(m);                    // the tab's schema is read on its owner thread
        if (m && v && !o.select.isEmpty()) {
            std::vector<std::string> unknown;
            const auto sel = m->selectColumns(o.select.toStdString(), &unknown);
            for (const auto& u : unknown)
                errs << tr("--select: no column matches '%1'").arg(QString::fromStdString(u));
            std::vector<int> disp;          // selected display columns, in spec order
            for (int fi : sel) {
                const QString name = QString::fromStdString(m->schema()->field(fi)->name());
                for (int c = 0; c < m->displayColumnCount(); ++c)
                    if (m->columnName(c) == name) { disp.push_back(c); break; }
            }
            if (!disp.empty()) {
                for (int c = 0; c < m->displayColumnCount(); ++c)
                    v->setColumnHidden(c, std::find(disp.begin(), disp.end(), c) == disp.end());
                auto* h = v->horizontalHeader();
                for (int k = 0; k < (int)disp.size(); ++k)
                    h->moveSection(h->visualIndex(disp[(size_t)k]), k);
            }
        }
        if (m && !o.filter.isEmpty()) {
            filterEdit_->setText(o.filter);
            applyFilter(/*ask=*/false);
            settle(m);
            if (!filterText_.contains(m))
                errs << tr("--filter: %1").arg(statusBar()->currentMessage());
        }
        if (m && !o.sort.isEmpty())
            if (const QString bad = sortActiveByNames(o.sort); !bad.isEmpty())
                errs << tr("--sort: no column '%1'").arg(bad);
        return errs;
    }
    // Set the horizontal scroll bar and read it back, with its maximum:
    // per-pixel scroll bars range over the table's pixel width and keep any
    // offset (mid-column); per-item bars range over the columns.
    std::pair<int, int> hscrollForTest(int value) {
        auto* v = activeView();
        if (!v) return {-1, -1};
        // Fixed geometry, so the result does not depend on the fonts
        // installed: 100-px columns in a 200-px view.
        for (int c = 0; c < v->model()->columnCount(); ++c)
            v->horizontalHeader()->resizeSection(c, 100);
        v->resize(200, 300);
        QCoreApplication::processEvents();
        v->horizontalScrollBar()->setValue(value);
        return {v->horizontalScrollBar()->value(), v->horizontalScrollBar()->maximum()};
    }
    // Under the limits `rows` / `bytes`, with every question answered
    // `answer`: a header click's sort, a filter, a find and a view state's
    // sort on the active tab — whether each asked ("asked" / "ran") and
    // whether it took effect.
    QString fullPassForTest(qint64 rows, qint64 bytes, bool answer) {
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v) return QStringLiteral("none");
        passRows_ = rows;
        passBytes_ = bytes;
        passAnswer_ = answer ? 1 : 0;
        auto wait = [m] {
            for (int g = 0; (m->isComputing() || !m->isReady()) && g < 2000000; ++g)
                QCoreApplication::processEvents();
        };
        wait();
        QStringList out;
        auto note = [&](const char* what, int before, int effect) {
            out << QStringLiteral("%1=%2,%3").arg(QLatin1String(what),
                       passAsked_ > before ? QStringLiteral("asked") : QStringLiteral("ran")).arg(effect);
        };
        int before = passAsked_;
        headerSortClick(m, v, 0, false);
        wait();
        note("sort", before, (int)m->sortKeys().size());
        if (!m->sortKeys().empty()) { setSortKeys(m, v, {}, false); wait(); }
        before = passAsked_;
        filterEdit_->setText(QStringLiteral("%1 is not null").arg(m->columnName(0)));
        applyFilter();
        wait();
        note("filter", before, m->hasFilter() ? 1 : 0);
        if (m->hasFilter()) { filterEdit_->clear(); applyFilter(); wait(); }
        before = passAsked_;
        const QRegularExpression re(QStringLiteral("."), QRegularExpression::CaseInsensitiveOption);
        findEdit_->setText(re.pattern());
        doFind(true);
        wait();
        note("find", before, m->findResultsValid(re) ? 1 : 0);
        m->clearSearch();
        TabViewState st;
        st.label = QString::fromStdString(m->tabLabel());
        st.sort = {{m->columnName(0), false}};
        applyViewState(m, v, st);
        wait();
        out << QStringLiteral("restore=%1").arg(m->sortKeys().size());
        passAnswer_ = -1;
        return out.join(QLatin1Char(' '));
    }
    // Sort the active tab by its first column (unasked), cancel after `ms`
    // and wait for what follows: the rows the sort read let go by a re-read
    // of the file ("reloaded=1") when it had read past the view's rows.
    QString cancelForTest(int ms) {
        auto* m = activeModel();
        auto* v = activeView();
        if (!m || !v) return QStringLiteral("none");
        for (int g = 0; !m->isReady() && g < 2000000; ++g) QCoreApplication::processEvents();
        const QString label = QString::fromStdString(m->tabLabel());
        const QPointer<ArrowTableModel> old(m);   // the re-read deletes it
        bool held = false;
        connect(m, &ArrowTableModel::heldAfterCancel, this, [&held](qint64) { held = true; });
        setSortKeys(m, v, {{0, Qt::AscendingOrder}}, /*ask=*/false);
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const bool running = m->isComputing();
        m->cancelRecompute();
        t.restart();
        // The re-read replaces the tab's model.
        while (t.elapsed() < 3000 && old) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        auto* now = activeModel();
        for (int g = 0; now && !now->isReady() && g < 2000000; ++g) QCoreApplication::processEvents();
        return QStringLiteral("running=%1 reloaded=%2 tab=%3 rows=%4")
            .arg(running ? 1 : 0).arg(held && !old ? 1 : 0)
            .arg(now ? QString::fromStdString(now->tabLabel()) == label : false).arg(now ? now->rowCount() : -1);
    }
    // Go to row `row` (0-based), with the first `frozen` columns frozen, and
    // scroll five rows down: the top row after the jump and after the scroll.
    QString gotoForTest(int row, int frozen) {
        auto* v = activeView();
        auto* m = activeModel();
        if (!v || !m) return QStringLiteral("none");
        v->resize(400, 300);
        if (frozen > 0) setFrozenColumns(v, frozen);
        QCoreApplication::processEvents();
        if (row >= m->rowCount()) return QStringLiteral("rows=%1").arg(m->rowCount());
        goToRow(v, row);
        QCoreApplication::processEvents();
        const int top = v->rowAt(0);
        // Five rows on, as a wheel turn does (the self-test's window is not
        // shown, so its page step is 0).
        QScrollBar* vs = v->verticalScrollBar();
        vs->setValue(vs->value() + (vs->maximum() > m->rowCount() ? 5 * v->rowHeight(row) : 5));
        QCoreApplication::processEvents();
        return QStringLiteral("top=%1 scrolled=%2").arg(top).arg(v->rowAt(0));
    }
    // Read the active stream tab as a user scrolling to the end would, up to
    // the load budget (and once more after Load More), then sample its rows
    // twice over: a batch the source let go reads empty.
    QString streamForTest() {
        auto* m = activeModel();
        if (!m) return QStringLiteral("none");
        QElapsedTimer t;
        t.start();
        while (!m->isReady() && t.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        auto toEnd = [&] {
            while (t.elapsed() < 20000) {
                while (m->isFetching() && t.elapsed() < 20000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                if (!m->canFetchMore({})) break;
                m->fetchMore({});
            }
        };
        toEnd();
        QString more;
        if (m->budgetReached()) {   // Load More reads one budget's worth on
            const int before = m->rowCount();
            loadMoreRows();
            toEnd();
            more = QStringLiteral(" more=%1").arg(m->rowCount() - before);
        }
        syncFileControls();
        // Every 1000th row, as a scroll to the end and back paints them.
        int blank = 0;
        {
            ArrowTableModel::SyncReads sync(m);
            for (int pass = 0; pass < 2; ++pass)
                for (int r = 0; r < m->rowCount(); r += 1000)
                    if (m->data(m->index(r, 0), Qt::DisplayRole).toString().isEmpty()) ++blank;
        }
        return QStringLiteral("rows=%1 partial=%2 bar=%3 blank=%4%5")
            .arg(m->rowCount()).arg(m->budgetReached() ? 1 : 0)
            .arg(contextBar_->partialShownForTest() ? 1 : 0).arg(blank).arg(more);
    }
    const char* scrollModeForTest() const {
        auto* v = activeView();
        if (!v) return "none";
        const bool px = v->horizontalScrollMode() == QAbstractItemView::ScrollPerPixel &&
                        v->verticalScrollMode()   == QAbstractItemView::ScrollPerPixel;
        return px ? "pixel" : "item";
    }
    void setExpandForTest(bool on) {
        newFileOptions_.expand = on;
    }
    // Every window-level shortcut key with the actions bound to it, and the
    // number of keys bound more than once (Qt treats those as ambiguous and
    // triggers none of the actions).
    // Window self-test: every leaf of the menu bar and of the ≡ menu, one
    // line each ("menubar File/Open…", "appmenu Open…"), and whether the
    // menu bar shows. Open Recent's entries and View ▸ Columns change with
    // the files, so only the submenus themselves are listed.
    void dumpMenusForTest() const {
        std::function<void(const char*, const QString&, const QList<QAction*>&)> walk =
            [&](const char* tree, const QString& at, const QList<QAction*>& actions) {
                for (QAction* a : actions) {
                    if (a->isSeparator()) continue;
                    QString name = a->text();
                    name.remove(QLatin1Char('&'));
                    const QString path = at.isEmpty() ? name : at + QLatin1Char('/') + name;
                    QMenu* sub = a->menu();
                    if (sub && sub != recentMenu_ && sub != columnsMenu_) walk(tree, path, sub->actions());
                    else std::printf("%s %s\n", tree, path.toUtf8().constData());
                }
            };
        walk("menubar", QString(), menuBar()->actions());
        walk("appmenu", QString(), appMenu_->actions());
        std::printf("menubar_visible=%d\n", menuBar()->isVisibleTo(this) ? 1 : 0);
    }
    int dumpShortcutsForTest() const {
        QMap<QString, QStringList> byKey;
        for (auto* a : findChildren<QAction*>()) {
            if (a->shortcutContext() == Qt::WidgetShortcut) continue;
            for (const auto& k : a->shortcuts())
                byKey[k.toString()] << a->text().remove(QLatin1Char('&'));
        }
        for (auto* sc : findChildren<QShortcut*>())
            for (const auto& k : sc->keys())
                byKey[k.toString()] << QStringLiteral("<QShortcut>");
        int conflicts = 0;
        for (auto it = byKey.cbegin(); it != byKey.cend(); ++it) {
            std::printf("shortcut %s -> %s\n", qPrintable(it.key()),
                        qPrintable(it.value().join(QStringLiteral(" | "))));
            if (it.value().size() > 1) ++conflicts;
        }
        std::printf("shortcut_conflicts=%d\n", conflicts);
        return conflicts;
    }
private:
    // Output format from the file extension (.parquet/.pq, .arrow/.feather/
    // .ipc, .tsv/.tab/.txt, .csv, .json, .ndjson/.jsonl).
    static bool exportFormatForPath(const QString& path, ExportFormat* f) {
        const QString ext = QFileInfo(path).suffix().toLower();
        if (ext == QLatin1String("parquet") || ext == QLatin1String("pq")) *f = ExportFormat::Parquet;
        else if (ext == QLatin1String("arrow") || ext == QLatin1String("feather") ||
                 ext == QLatin1String("ipc"))                            *f = ExportFormat::Arrow;
        else if (ext == QLatin1String("tsv") || ext == QLatin1String("tab") ||
                 ext == QLatin1String("txt"))                            *f = ExportFormat::Tsv;
        else if (ext == QLatin1String("csv"))                             *f = ExportFormat::Csv;
        else if (ext == QLatin1String("json"))                            *f = ExportFormat::Json;
        else if (ext == QLatin1String("ndjson") || ext == QLatin1String("jsonl")) *f = ExportFormat::Ndjson;
        else return false;
        return true;
    }

    void exportViewDialog(bool selection = false) {
        if (exportWatcher_) return;                 // one export at a time
        VvCommandSpec s;
        if (!specForActiveTab(&s)) return;
        if (selection && !restrictToSelection(&s)) return;
        struct Choice { QString filter, ext; };
        const Choice choices[] = {
            {tr("Parquet (*.parquet)"),          QStringLiteral("parquet")},
            {tr("Arrow IPC / Feather (*.arrow)"), QStringLiteral("arrow")},
            {tr("Tab-separated (*.tsv)"),         QStringLiteral("tsv")},
            {tr("Comma-separated (*.csv)"),       QStringLiteral("csv")},
            {tr("JSON array (*.json)"),           QStringLiteral("json")},
            {tr("JSON lines (*.ndjson)"),         QStringLiteral("ndjson")},
        };
        QStringList filters;
        for (const auto& c : choices) filters << c.filter;
        QString chosen = filters.first();
        const QString base = QFileInfo(s.path).completeBaseName()
                             + (s.tab.isEmpty() ? QString() : QLatin1Char('.') + s.tab)
                             + (selection ? QStringLiteral(".selection") : QString());
        QString out = QFileDialog::getSaveFileName(
            this, selection ? tr("Export selection") : tr("Export view"),
            QDir(lastDir_.isEmpty() ? QDir::currentPath() : lastDir_)
                .filePath(base + QStringLiteral(".parquet")),
            filters.join(QStringLiteral(";;")), &chosen);
        if (out.isEmpty()) return;
        ExportFormat f;
        if (!exportFormatForPath(out, &f)) {   // no / unknown extension: the chosen filter's
            for (const auto& c : choices)
                if (c.filter == chosen) { out += QLatin1Char('.') + c.ext; break; }
            if (!exportFormatForPath(out, &f)) return;
        }
        startExport(s, out, f);
    }

    // Limit an export of the active tab to the selection: its rows (as source
    // rows) and, when it does not span every visible column, its columns.
    bool restrictToSelection(VvCommandSpec* s) const {
        auto* v = activeView();
        auto* m = activeModel();
        if (!v || !m) return false;
        const SelectionGrid g = selectionGrid(v, m, false);
        if (g.rows.empty()) return false;
        for (int r : g.rows) s->rows.push_back(m->sourceRowOf(r));
        int visible = 0;
        for (int c = 0; c < m->columnCount(); ++c) visible += !v->isColumnHidden(c);
        if ((int)g.cols.size() < visible) {
            s->select.clear();
            for (int c : g.cols) s->select << selectTerm(m, c);
        }
        return true;
    }
    // A display column as a --select term: its name, or its 1-based source
    // position ("N-N") when --select would split or trim the name.
    static QString selectTerm(ArrowTableModel* m, int c) {
        const QString name = m->columnName(c);
        if (name.contains(QLatin1Char(',')) || name.trimmed() != name || name.isEmpty())
            return QStringLiteral("%1-%1").arg(m->sourceColumn(c) + 1);
        return name;
    }

    // Run export_view on a worker thread; the status bar shows the rows written
    // and the Cancel button stops it (the partial file is removed).
    void startExport(const VvCommandSpec& spec, const QString& out, ExportFormat f) {
        exportProgress_ = std::make_shared<ExportProgress>();
        exportPath_ = out;
        const Config cfg = vvCommandConfig(spec);
        const std::string outPath = out.toStdString();
        auto prog = exportProgress_;
        exportWatcher_ = new QFutureWatcher<QString>(this);
        connect(exportWatcher_, &QFutureWatcher<QString>::finished, this, [this]{ onExportDone(); });
        exportWatcher_->setFuture(QtConcurrent::run([cfg, outPath, f, prog] {
            return QString::fromStdString(export_view(cfg, outPath, f, prog.get()));
        }));
        if (exportAction_) exportAction_->setEnabled(false);
        progress_->setVisible(true);
        cancelBtn_->setVisible(true);
        if (!exportTimer_) {
            exportTimer_ = new QTimer(this);
            exportTimer_->setInterval(250);
            connect(exportTimer_, &QTimer::timeout, this, [this]{
                if (exportProgress_)
                    statusBar()->showMessage(tr("Exporting to %1 … %L2 rows")
                        .arg(exportPath_).arg((qlonglong)exportProgress_->rows.load()));
            });
        }
        exportTimer_->start();
        statusBar()->showMessage(tr("Exporting to %1 …").arg(exportPath_));
    }

    void onExportDone() {
        lastExportError_ = exportWatcher_->result();
        lastExportRows_  = exportProgress_ ? exportProgress_->rows.load() : 0;
        exportWatcher_->deleteLater();
        exportWatcher_ = nullptr;
        exportProgress_.reset();
        if (exportTimer_) exportTimer_->stop();
        if (exportAction_) exportAction_->setEnabled(true);
        if (!computingModel_) {
            progress_->setVisible(false);
            cancelBtn_->setVisible(false);
        }
        if (lastExportError_.isEmpty()) {
            if (openExportDirAct_) openExportDirAct_->setEnabled(true);
            statusBar()->showMessage(tr("Exported %L1 rows to %2")
                                     .arg(lastExportRows_).arg(exportPath_), 8000);
        } else if (lastExportError_ == QLatin1String("canceled")) {
            statusBar()->showMessage(tr("Export canceled"), 4000);
        } else if (headless_) {
            std::fprintf(stderr, "vvg export: %s\n", lastExportError_.toStdString().c_str());
        } else {
            statusBar()->clearMessage();
            QMessageBox::warning(this, tr("vvg — export"), lastExportError_);
        }
    }

    void copyCommand() {
        const QString cmd = commandForActiveTab();
        if (cmd.isEmpty()) return;
        QApplication::clipboard()->setText(cmd);
        statusBar()->showMessage(tr("Copied: %1").arg(cmd), 4000);
    }

    // Right-click menu on the grid: Copy (cell/selection) and Copy Row.
    void showTableContextMenu(QTableView* view, const QPoint& pos) {
        QModelIndex idx = view->indexAt(pos);
        // Right-clicking a cell outside the current selection acts on that cell.
        if (idx.isValid() && !view->selectionModel()->isSelected(idx)) {
            view->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
            view->setCurrentIndex(idx);
        }
        QMenu menu(view);
        QAction* copyCell = menu.addAction(tr("&Copy"));
        copyCell->setShortcut(QKeySequence::Copy);
        connect(copyCell, &QAction::triggered, this, [this]{ copySelection(); });
        QAction* copyRow = menu.addAction(tr("Copy &Row"));
        connect(copyRow, &QAction::triggered, this, [this]{ copyRows(); });
        menu.addSeparator();
        QAction* copyCmd = menu.addAction(tr("Copy as &vv Command"));
        connect(copyCmd, &QAction::triggered, this, [this]{ copyCommand(); });
        if (idx.isValid()) {
            // Filter by the selected values of the clicked column (that cell
            // alone when the selection spans other columns).
            auto* m = qobject_cast<ArrowTableModel*>(view->model());
            const QList<QModelIndex> cells = cellsForFilter(view, idx);
            const QString what = cellFilterLabel(m, cells);
            menu.addSeparator();
            connect(menu.addAction(tr("&Filter to %1").arg(what)), &QAction::triggered, this,
                    [this, m, cells] { applyCellFilter(m, cells, false); });
            connect(menu.addAction(tr("&Exclude %1").arg(what)), &QAction::triggered, this,
                    [this, m, cells] { applyCellFilter(m, cells, true); });
            connect(menu.addAction(tr("Column &Statistics")), &QAction::triggered, this,
                    [this, view, idx] { view->setCurrentIndex(idx); showColumnTab(); });
            if (const OpenFile* f = activeFile(); f && m)
                addGgAction(menu, f, rowLocus(m, idx.row(), f->feat.format));
            menu.addSeparator();
            menu.addMenu(copyAsMenu_);
            menu.addAction(exportSelAct_);
        }
        menu.exec(view->viewport()->mapToGlobal(pos));
    }

    // The cells a cell filter takes: the selected cells of `clicked`'s
    // column, or `clicked` alone when the selection spans other columns.
    static QList<QModelIndex> cellsForFilter(QTableView* view, const QModelIndex& clicked) {
        QList<QModelIndex> sel = view->selectionModel()->selectedIndexes();
        for (const QModelIndex& i : sel)
            if (i.column() != clicked.column()) return {clicked};
        return sel.isEmpty() ? QList<QModelIndex>{clicked} : sel;
    }
    // "“chr1”", "empty cells" or "3 values", for the menu.
    QString cellFilterLabel(ArrowTableModel* m, const QList<QModelIndex>& cells) const {
        std::vector<ValueCount> vs = cellValues(m, cells);
        if (vs.size() == 1) {
            if (vs[0].null) return tr("Empty Cells");
            return QStringLiteral("\u201C%1\u201D").arg(
                fontMetrics().elidedText(QString::fromStdString(vs[0].value), Qt::ElideMiddle, 240));
        }
        return tr("%n Values", nullptr, (int)vs.size());
    }
    // The distinct values of `cells` (at most 200), nulls once.
    static std::vector<ValueCount> cellValues(ArrowTableModel* m, const QList<QModelIndex>& cells) {
        std::vector<ValueCount> out;
        ArrowTableModel::SyncReads sync(m);
        for (const QModelIndex& i : cells) {
            bool ok = false;
            ValueCount v = m->filterValue(i.row(), i.column(), &ok);
            if (!ok) continue;
            const bool dup = std::any_of(out.begin(), out.end(), [&](const ValueCount& o) {
                return o.null == v.null && o.value == v.value;
            });
            if (!dup) out.push_back(v);
            if (out.size() >= 200) break;
        }
        return out;
    }
    // Filter to (or exclude) the values of `cells`, ANDed into the current
    // filter. Returns the expression applied, or "" with the reason shown.
    QString applyCellFilter(ArrowTableModel* m, const QList<QModelIndex>& cells, bool exclude) {
        if (!m || cells.isEmpty()) return {};
        const std::vector<ValueCount> vs = cellValues(m, cells);
        std::string expr, why;
        if (vs.empty() || !narrow_filter_by_values(activeFilterText(m).toStdString(), *m->schema(),
                                                   m->sourceColumn(cells.first().column()), vs,
                                                   exclude, &expr, &why)) {
            statusBar()->showMessage(tr("filter: %1").arg(
                QString::fromStdString(why.empty() ? std::string("the rows are not loaded") : why)), 4000);
            return {};
        }
        filterEdit_->setText(QString::fromStdString(expr));
        applyFilter();
        return QString::fromStdString(expr);
    }

    void refreshStatus() {
        if (JsonTab* jt = activeJsonTab()) {
            const QModelIndex cur = jt->view->currentIndex();
            statusBar()->showMessage(jt->model->summary() +
                                     (cur.isValid() ? QStringLiteral("  |  ") + jt->model->path(cur) : QString()));
            return;
        }
        if (auto* m = activeModel())
            statusBar()->showMessage(m->footer());
    }

    // ── JSON tree tabs ──────────────────────────────────────────────────────
    struct JsonTab {
        JsonTreeModel* model = nullptr;      // owned by the view's parent chain (deleted with it)
        QTreeView*     view  = nullptr;
        QString        path;
        std::unique_ptr<QTemporaryFile> tmp; // a compressed file's decompressed copy
        // Find: a scan of the mapped bytes off the UI thread.
        QFutureWatcher<uint64_t>* finder = nullptr;   // child of the view
        std::atomic<bool>     findCancel{false};
        std::atomic<uint64_t> findPos{0};
        bool findWrapped = false, findCancelled = false;
        bool searching() const { return finder && finder->isRunning(); }
        ~JsonTab() {                         // the scan reads the document: stop it first
            findCancel = true;
            if (finder) finder->waitForFinished();
        }
    };
    std::map<QWidget*, std::unique_ptr<JsonTab>> jsonTabs_;
    // Tabs not shown yet: placeholder widget -> the source and how it opened.
    struct PendingTab {
        std::unique_ptr<TabularSource> src;
        QString                        origin;
        bool                           primary = true;
        QString                        expand;
        std::shared_ptr<SourceOwner>   owner;
        int                            fileId = 0;
    };
    std::map<QWidget*, PendingTab>               pending_;
    bool jsonAsTable_ = false;

    JsonTab* activeJsonTab() const {
        auto it = jsonTabs_.find(tabs_->currentWidget());
        return it == jsonTabs_.end() ? nullptr : it->second.get();
    }
    // .json / .geojson / .ipynb / .har, optionally .gz / .bgz / .zst
    // (a document; .ndjson / .jsonl records stay tables).
    static bool jsonDocumentPath(const QString& path) {
        QString p = path.toLower();
        for (const char* z : {".gz", ".bgz", ".zst", ".zstd"})
            if (p.endsWith(QLatin1String(z))) { p.chop((int)std::strlen(z)); break; }
        return p.endsWith(QLatin1String(".json")) || p.endsWith(QLatin1String(".geojson")) ||
               p.endsWith(QLatin1String(".ipynb")) || p.endsWith(QLatin1String(".har"));
    }
    // Decompress a .gz / .zst file to a temporary file (the tree maps its input).
    static QString decompressTo(const QString& path, QTemporaryFile* out) {
        auto raw = arrow::io::ReadableFile::Open(path.toStdString());
        if (!raw.ok()) return QString::fromStdString(raw.status().ToString());
        uint8_t magic[4] = {0, 0, 0, 0};
        auto got = (*raw)->ReadAt(0, 4, magic);
        if (!got.ok()) return QString::fromStdString(got.status().ToString());
        arrow::Compression::type comp = arrow::Compression::UNCOMPRESSED;
        if (magic[0] == 0x1f && magic[1] == 0x8b) comp = arrow::Compression::GZIP;
        else if (magic[0] == 0x28 && magic[1] == 0xb5 && magic[2] == 0x2f && magic[3] == 0xfd)
            comp = arrow::Compression::ZSTD;
        if (comp == arrow::Compression::UNCOMPRESSED) return QStringLiteral("-");   // map it directly
        if (auto st = (*raw)->Seek(0); !st.ok()) return QString::fromStdString(st.ToString());
        auto codec = arrow::util::Codec::Create(comp);
        if (!codec.ok()) return QString::fromStdString(codec.status().ToString());
        auto in = arrow::io::CompressedInputStream::Make(codec->get(), *raw);
        if (!in.ok()) return QString::fromStdString(in.status().ToString());
        if (!out->open()) return QObject::tr("cannot create a temporary file");
        for (;;) {
            auto buf = (*in)->Read(1 << 20);
            if (!buf.ok()) return QString::fromStdString(buf.status().ToString());
            if ((*buf)->size() == 0) break;
            if (out->write(reinterpret_cast<const char*>((*buf)->data()), (*buf)->size()) != (*buf)->size())
                return QObject::tr("writing the decompressed copy failed");
        }
        out->flush();
        return {};
    }
    // Open `path` as a JSON tree tab; "" or an error.
    QString openJsonTreeTab(const QString& path, int fileId) {
        auto jt = std::make_unique<JsonTab>();
        jt->path = path;
        QString file = path;
        auto tmp = std::make_unique<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/vvg-XXXXXX.json"));
        const QString d = decompressTo(path, tmp.get());
        if (d.isEmpty()) { file = tmp->fileName(); jt->tmp = std::move(tmp); }
        else if (d != QStringLiteral("-")) return path + QStringLiteral(": ") + d;
        auto doc = std::make_unique<vvjson::JsonDoc>();
        const std::string e = doc->open(file.toStdString(), /*lines=*/false);
        if (!e.empty()) return QString::fromStdString(e);
        jt->model = new JsonTreeModel(std::move(doc), this);
        auto* view = new QTreeView(tabs_);
        jt->view = view;
        jt->model->setParent(view);
        view->setModel(jt->model);
        view->setUniformRowHeights(true);
        view->setAlternatingRowColors(true);
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        view->header()->setStretchLastSection(false);
        view->header()->setSectionResizeMode(JsonTreeModel::Value, QHeaderView::Stretch);
        view->setColumnWidth(JsonTreeModel::Key, 260);
        view->setColumnWidth(JsonTreeModel::Type, 90);
        const QModelIndex root = jt->model->index(0, 0);
        view->expand(root);
        view->setCurrentIndex(root);
        JsonTab* raw = jt.get();
        connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this, raw](const QModelIndex& cur, const QModelIndex&) {
                    refreshStatus();
                    if (activeJsonTab() == raw && detail_) updateJsonDetail(raw, cur);
                });
        connect(view, &QWidget::customContextMenuRequested, this,
                [this, raw](const QPoint& pos) { showJsonContextMenu(raw, pos); });
        // While the background validator runs, keep the status line current.
        auto* timer = new QTimer(view);
        connect(timer, &QTimer::timeout, this, [this, raw, timer] {
            if (activeJsonTab() == raw) refreshStatus();
            if (raw->model->doc().validation() != 0) timer->stop();
        });
        timer->start(300);
        views_.push_back(nullptr);            // index-aligned with the tabs; not a table
        models_.push_back(nullptr);
        jsonTabs_[view] = std::move(jt);
        tabFile_[view] = fileId;
        const int idx = tabs_->addTab(view, QFileInfo(path).fileName());
        tabs_->setTabToolTip(idx, path);
        tabs_->setCurrentIndex(idx);
        return {};
    }
    QModelIndex findJsonPath(JsonTab* jt, const QString& path) {
        // Walk from the root, expanding (fetching) until the path matches.
        QModelIndex at = jt->model->index(0, 0);
        for (int guard = 0; guard < 100000; ++guard) {
            if (jt->model->path(at) == path) return at;
            jt->view->expand(at);
            while (jt->model->canFetchMore(at)) jt->model->fetchMore(at);
            bool stepped = false;
            for (int r = 0; r < jt->model->rowCount(at); ++r) {
                const QModelIndex c = jt->model->index(r, 0, at);
                const QString cp = jt->model->path(c);
                if (path == cp || path.startsWith(cp + QLatin1Char('.')) || path.startsWith(cp + QLatin1Char('['))) {
                    at = c;
                    stepped = true;
                    break;
                }
            }
            if (!stepped) return {};
        }
        return {};
    }
    void openRecordsTable(JsonTab* jt, const QModelIndex& at) {
        const QModelIndex rec = jt->model->recordsAt(at);
        std::unique_ptr<TabularSource> src;
        std::string e;
        const QString label = QFileInfo(jt->path).fileName() + QLatin1Char(' ') +
                              (rec.isValid() ? jt->model->path(rec) : QStringLiteral("."));
        if (rec.isValid()) {
            const std::string_view b = jt->model->doc().bytes(jt->model->node(rec));
            auto buf = std::make_shared<arrow::Buffer>(reinterpret_cast<const uint8_t*>(b.data()),
                                                       (int64_t)b.size());
            e = open_json_records(std::make_shared<arrow::io::BufferReader>(buf), label.toStdString(), &src);
        } else {
            Config cfg;
            e = open_source(jt->path.toStdString(), cfg, &src);
        }
        if (!e.empty()) {
            const QString msg = tr("No table view: %1").arg(QString::fromStdString(e));
            if (headless_) std::fprintf(stderr, "vvg: %s\n", msg.toLocal8Bit().constData());
            else QMessageBox::information(this, tr("vvg — JSON"), msg);
            return;
        }
        ArrowTableModel* tm = addSourceTab(std::move(src), jt->path, true, QString(), {}, 0, /*now=*/true);
        if (rec.isValid()) tabOrigin_[tm].reopen = false;
        tabs_->setCurrentIndex(tabs_->count() - 1);
    }
    void showJsonContextMenu(JsonTab* jt, const QPoint& pos) {
        const QModelIndex at = jt->view->indexAt(pos);
        if (!at.isValid()) return;
        QMenu menu(this);
        QAction* copyValue = menu.addAction(tr("Copy Value"));
        QAction* copyPath = menu.addAction(tr("Copy Path"));
        menu.addSeparator();
        const QModelIndex rec = jt->model->recordsAt(at);
        QAction* table = menu.addAction(rec.isValid() ? tr("Open %1 as Table").arg(jt->model->path(rec))
                                                      : tr("Open Document as Table"));
        QAction* chosen = menu.exec(jt->view->viewport()->mapToGlobal(pos));
        if (chosen == copyValue) QGuiApplication::clipboard()->setText(jt->model->valueText(at));
        else if (chosen == copyPath) QGuiApplication::clipboard()->setText(jt->model->path(at));
        else if (chosen == table) openRecordsTable(jt, at);
    }

    void buildProgressUI() {
        progress_ = new QProgressBar(this);
        progress_->setRange(0, 0);          // indeterminate "busy" spinner
        progress_->setMaximumWidth(160);
        progress_->setVisible(false);
        cancelBtn_ = new QPushButton(tr("Cancel"), this);
        cancelBtn_->setVisible(false);
        connect(cancelBtn_, &QPushButton::clicked, this, [this]{
            if (exportProgress_) exportProgress_->cancel = true;
            else if (JsonTab* jt = activeJsonTab(); jt && jt->searching()) jt->findCancel = true;
            else if (computingModel_) computingModel_->cancelRecompute();
        });
        // The selection summary is a permanent widget so that timed status
        // messages and refreshStatus() leave it alone; selectable, to copy.
        selSum_ = new QLabel(this);
        selSum_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        selSum_->setVisible(false);
        selTimer_ = new QTimer(this);
        selTimer_->setSingleShot(true);
        selTimer_->setInterval(100);
        connect(selTimer_, &QTimer::timeout, this, [this]{ updateSelectionSummary(); });
        statusBar()->addPermanentWidget(selSum_);
        statusBar()->addPermanentWidget(progress_);
        statusBar()->addPermanentWidget(cancelBtn_);
    }

    // Σ of the numeric cells selected in the active table: sum, mean, min, max
    // and how many numbers, from the cells themselves (hidden columns and
    // text skipped). Shown for two or more selected cells.
    void updateSelectionSummary() {
        auto hide = [this]{ selSum_->clear(); selSum_->setVisible(false); };
        auto* v = activeView();
        auto* m = activeModel();
        if (!v || !m || m->isComputing() || !v->selectionModel()) return hide();
        const QItemSelection sel = v->selectionModel()->selection();
        qint64 cells = 0;
        for (const auto& r : sel) cells += (qint64)r.width() * r.height();
        if (cells < 2) return hide();
        if (cells > kSelectionSumMaxCells) {
            selSum_->setText(tr("Σ: more than %L1 cells selected — see the Column tab")
                                 .arg(kSelectionSumMaxCells));
            selSum_->setVisible(true);
            return;
        }
        ArrowTableModel::SyncReads now(m);   // the selection's rows, loaded or not (capped above)
        long double sum = 0;
        double mn = 0, mx = 0;
        qint64 n = 0;
        int digits = 19;
        for (int i = 0; i < sel.size(); ++i) {
            const QItemSelectionRange& r = sel[i];
            for (int c = r.left(); c <= r.right(); ++c) {
                const int dg = m->summableDigits(c);
                if (dg == 0 || v->isColumnHidden(c)) continue;
                for (int row = r.top(); row <= r.bottom(); ++row) {
                    // Ranges may overlap (Ctrl+click): count a cell once.
                    bool seen = false;
                    for (int j = 0; j < i && !seen; ++j)
                        seen = sel[j].contains(row, c, QModelIndex());
                    if (seen) continue;
                    const QVariant x = m->data(m->index(row, c), ArrowTableModel::NumericRole);
                    if (!x.isValid()) continue;
                    const double d = x.toDouble();
                    if (n == 0 || d < mn) mn = d;
                    if (n == 0 || d > mx) mx = d;
                    sum += d;
                    ++n;
                    digits = std::min(digits, dg);
                }
            }
        }
        if (n == 0) return hide();
        auto num = [](long double x, int dg) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.*Lg", dg, x);
            return QString::fromLatin1(buf);
        };
        // An integer column's mean is not an integer: at most 15 digits.
        selSum_->setText(tr("Σ %1 · mean %2 · min %3 · max %4 · %L5 numbers")
                             .arg(num(sum, digits), num(sum / n, std::min(digits, 15)),
                                  num(mn, digits), num(mx, digits))
                             .arg(n));
        selSum_->setVisible(true);
    }
    void onRecomputeStarted(ArrowTableModel* m) {
        computingModel_ = m;
        selSum_->setVisible(false);
        progress_->setVisible(true);
        cancelBtn_->setVisible(true);
        statusBar()->showMessage(tr("Working…"));
    }
    void onRecomputeFinished(ArrowTableModel* m) {
        if (m == computingModel_) computingModel_ = nullptr;
        if (colTimer_) colTimer_->start();
        progress_->setVisible(false);
        cancelBtn_->setVisible(false);
        if (m != activeModel()) return;
        if (m->hasFilter())
            statusBar()->showMessage(tr("filter: %1 / %2 rows  —  %3")
                .arg((qlonglong)m->viewRows())
                .arg((qlonglong)m->sourceTotal())
                .arg(m->footer()));
        else
            refreshStatus();
    }
    void onRecomputeStopped() {           // canceled
        computingModel_ = nullptr;
        progress_->setVisible(false);
        cancelBtn_->setVisible(false);
        statusBar()->showMessage(tr("canceled"), 2000);
    }

    TabPane*                       tabs_   = nullptr;
    ContextBar*                    contextBar_ = nullptr;   // the current tab's format options
    QTableWidget*                  detail_ = nullptr;
    QPointer<ArrowTableModel>      detailModel_;      // model / row the dock shows
    int                            detailRow_ = -1;
    QLineEdit*                     filterEdit_ = nullptr;
    QLineEdit*                     findEdit_   = nullptr;
    bool                           smoothScroll_  = true;   // per-pixel scrolling
    bool                           showTypes_     = true;   // the header's type line
    QAction*                       typesAct_      = nullptr;   // View ▸ Column Types
    QAction*                       closeFileAct_  = nullptr;   // File ▸ Close File
    QAction*                       reloadAct_     = nullptr;   // View ▸ Reload (F5)
    QAction*                       reopenClosedAct_ = nullptr; // File ▸ Reopen Closed Tab
    QAction*                       restoreSessionAct_ = nullptr;
    bool                           restoreSession_ = true;   // ui/restoreSession
    // Bytes of a stream's batches a tab reads before it stops and offers
    // Load More (limits/loadBytes; ArrowTableModel::setLoadBudget).
    qint64                         loadBudget_ = ArrowTableModel::kDefaultLoadBudget;
    QAction*                       loadMoreAct_ = nullptr;   // View ▸ Load More Rows
    // A sort, filter or find over more rows than this, or a file of more
    // bytes while its row count is unknown, asks first (confirmFullPass;
    // limits/passRows, limits/passBytes). Column statistics use the same.
    qint64                         passRows_  = ArrowTableModel::kDefaultPassRows;
    qint64                         passBytes_ = ArrowTableModel::kDefaultPassBytes;
    int                            passAsked_ = 0;     // questions asked (self-test)
    int                            passAnswer_ = -1;   // self-test: 1 yes, 0 no; -1 ask
    QList<ClosedTab>               closedTabs_;             // newest last, 20 kept
    QAction*                       openAct_       = nullptr;   // toolbar + File
    QAction*                       openDirAct_    = nullptr;
    QAction*                       openUrlAct_    = nullptr;   // File ▸ Open URL
    QAction*                       copyAct_       = nullptr;   // toolbar + Edit
    QAction*                       copyRowAct_    = nullptr;
    QMenu*                         copyAsMenu_    = nullptr;   // Edit ▸ Copy As
    QList<QAction*>                copyAsActs_;
    QAction*                       exportSelAct_  = nullptr;   // Export Selection As…
    QStringList                    recent_;                 // Open Recent, newest first
    QString                        openFilter_;             // the Open dialog's last type
    QHash<QString, QStringList>    recentFilters_;          // family key -> newest first
    QHash<QString, QVariantMap>    savedFilters_;           // family key -> name -> expression
    QStringListModel*              filterHistModel_ = nullptr;   // the Filter box's completions
    QHash<QWidget*, TabViewState>  pendingState_;           // placeholder -> state to give it
    QHash<ArrowTableModel*, QPair<qint64, int>> pendingCursor_;   // row, column once loaded
    QHash<int, FileStamp>          stamps_;                 // file id -> size / mtime when read
    QSet<int>                      changedFiles_;           // changed on disk since
    QFileSystemWatcher*            watcher_    = nullptr;
    QTimer*                        watchTimer_ = nullptr;   // debounces the watcher
    QThread*                       settingsThread_ = nullptr;   // writes vvg.conf (saveSetting)
    QObject*                       settingsCtx_    = nullptr;   // lives on settingsThread_
    QMenu*                         recentMenu_  = nullptr;
    QMenu*                         columnsMenu_ = nullptr;
    static constexpr qint64        kSelectionSumMaxCells = 1'000'000;
    QLabel*                        selSum_     = nullptr;   // status-bar Σ of the selection
    QTimer*                        selTimer_   = nullptr;   // coalesces selection changes
    QProgressBar*                  progress_   = nullptr;
    QPushButton*                   cancelBtn_  = nullptr;
    ArrowTableModel*               computingModel_ = nullptr;  // model with a live worker
    ArrowTableModel*               pendingFindModel_ = nullptr;  // awaiting findReady
    int                            pendingFindRow_ = -1;
    int                            pendingFindCol_ = -1;
    bool                           pendingFindForward_ = true;
    QString                        lastDir_;
    bool                           headless_ = false;   // suppress modal dialogs
    // The open files and their format options; each tab maps to its file
    // (tabFile_: tab widget -> OpenFile::id, 0 for a tab of no file).
    std::vector<OpenFile>          files_;
    QHash<QWidget*, int>           tabFile_;
    int                            nextFileId_ = 1;
    FileOptions                    newFileOptions_;   // a file's options when it opens
    // The toolbar row and the menus (buildMenus / buildToolbar).
    QToolBar*    mainBar_        = nullptr;
    QWidget*     searchGroup_    = nullptr;   // the Filter and Find boxes
    QToolButton* detailsButton_  = nullptr;
    QToolButton* exportButton_   = nullptr;
    QToolButton* appButton_      = nullptr;   // ≡
    QMenu*       appMenu_        = nullptr;
    QMenu*       exportMenu_     = nullptr;
    QAction*     detailsAct_     = nullptr;
    QAction*     copyCmdAct_     = nullptr;
    QAction*     openExportDirAct_ = nullptr;
    QAction*     showMenuBarAct_ = nullptr;
    QAction*     showAppMenuAct_ = nullptr;
    int          compactLevel_   = 0;
    struct IconAction { QAction* action; const char* themeName; Glyph glyph; };
    std::vector<IconAction> iconActions_;
    std::vector<QTableView*>       views_;
    std::vector<ArrowTableModel*>  models_;
    // reopen: the tab can be opened again from `path` (not stdin / a FIFO, not
    // a table made from a JSON-tree node).
    QMenu*                                viewMenu_  = nullptr;
    QDockWidget*                          detailsDock_ = nullptr;
    QTabWidget*                           detailTabs_  = nullptr;   // Row | Column
    ColumnPanel*                          colPanel_  = nullptr;
    QTimer*                               colTimer_  = nullptr;   // debounces cursor moves
    QHash<ArrowTableModel*, int>          keepCol_;   // cursor column across a model reset
    QTimer*                               statsPoll_ = nullptr;   // progress of a scan
    QFutureWatcher<StatsResult>*          statsWatcher_ = nullptr;
    QList<QFuture<StatsResult>>           statsFutures_;          // joined on close
    std::shared_ptr<ExportProgress>       statsProg_;             // the current scan
    QPointer<ArrowTableModel>             statsModel_;
    QString                               statsFilter_;
    std::vector<int>                      statsCols_;
    QMap<ArrowTableModel*, StatsCache>    statsCache_;
    struct TabOrigin { QString path; bool primary = true; QString expand; bool reopen = true; };
    QMap<ArrowTableModel*, TabOrigin>     tabOrigin_;    // file + first-tab flag
    QMap<ArrowTableModel*, QString>       filterText_;   // applied --filter text
    // Export View As…: one export at a time, on a worker thread.
    QAction*                              exportAction_   = nullptr;
    QFutureWatcher<QString>*              exportWatcher_  = nullptr;
    std::shared_ptr<ExportProgress>       exportProgress_;
    QTimer*                               exportTimer_    = nullptr;
    QString                               exportPath_;
    QString                               lastExportError_;
    qint64                                lastExportRows_ = 0;
public:
    // Closing the window mid-export stops the worker (the partial file is
    // removed) before the window's state goes away.
    ~MainWindow() override {
        if (exportWatcher_) {
            if (exportProgress_) exportProgress_->cancel = true;
            exportWatcher_->waitForFinished();
        }
        cancelColumnStats();
        for (auto& f : statsFutures_) f.waitForFinished();
        // The writes queued so far run first, then the thread stops.
        QMetaObject::invokeMethod(settingsCtx_, [] { QThread::currentThread()->quit(); },
                                  Qt::QueuedConnection);
        settingsThread_->wait();
    }
    // Self-test: the active tab's column widths, in multiples of the width
    // of '0' (font-independent), e.g. "ch=8 widths=14.0,62.5".
    QString colWidthsForTest() {
        waitIdleForTest();
        auto* v = activeView();
        auto* m = activeModel();
        if (!v || !m) return QStringLiteral("no table");
        const double ch = std::max(1, v->fontMetrics().horizontalAdvance(QLatin1Char('0')));
        QStringList w;
        for (int c = 0; c < m->columnCount(); ++c)
            w << QString::number(v->horizontalHeader()->sectionSize(c) / ch, 'f', 1);
        return QStringLiteral("widths=") + w.join(QLatin1Char(','));
    }
    // Every command for Help ▸ Keyboard Shortcuts, with the menu it is in
    // ("File", "View ▸ Columns", …); commands in no menu are "Window".
    ShortcutsDialog::Commands commandsForHelp() const {
        ShortcutsDialog::Commands out;
        QSet<QAction*> seen;
        std::function<void(QMenu*, const QString&)> walk = [&](QMenu* m, const QString& where) {
            if (m == recentMenu_ || m == columnsMenu_) return;   // lists of files / columns
            for (QAction* a : m->actions()) {
                if (a->isSeparator()) continue;
                if (QMenu* sub = a->menu()) {
                    walk(sub, where + QStringLiteral(" \u25B8 ") + sub->title().remove(QLatin1Char('&')));
                    continue;
                }
                if (seen.contains(a)) continue;
                seen.insert(a);
                out << qMakePair(where, a);
            }
        };
        for (QAction* top : menuBar()->actions())
            if (QMenu* m = top->menu()) walk(m, m->title().remove(QLatin1Char('&')));
        if (exportMenu_) walk(exportMenu_, tr("Export"));
        for (QAction* a : actions())
            if (!seen.contains(a) && !a->text().isEmpty()) out << qMakePair(tr("Window"), a);
        return out;
    }
    // Help ▸ Find Action: the palette under the toolbar's centre.
    ActionPalette* openPalette(const QString& query = {}) {
        auto* p = new ActionPalette(commandsForHelp(), this);
        if (!query.isEmpty()) p->setQuery(query);
        const QWidget* anchor = mainBar_ ? static_cast<QWidget*>(mainBar_) : this;
        p->popupAt(anchor->mapToGlobal(QPoint(anchor->width() / 2, anchor->height())));
        return p;
    }
    // Self-test: Filter to / Exclude the cells "row,col[,row,col…]" of the
    // active tab; the expression and the rows left.
    QString cellFilterForTest(const QString& spec, bool exclude) {
        waitIdleForTest();
        auto* v = activeView();
        auto* m = activeModel();
        if (!v || !m) return QStringLiteral("no table");
        const QStringList n = spec.split(QLatin1Char(','));
        QList<QModelIndex> cells;
        for (int i = 0; i + 1 < n.size(); i += 2) cells << m->index(n[i].toInt(), n[i + 1].toInt());
        const QString expr = applyCellFilter(m, cells, exclude);
        waitIdleForTest();
        return QStringLiteral("%1 -> rows=%2").arg(expr).arg(m->rowCount());
    }
    // Self-test: header clicks "3,+0,+0" ('+' = with Shift) on the active
    // tab; after each, the sort keys and the header marks.
    void headerSortForTest(const QString& clicks) {
        waitIdleForTest();
        auto* v = activeView();
        auto* m = activeModel();
        if (!v || !m) return;
        for (QString c : clicks.split(QLatin1Char(','))) {
            const bool add = c.startsWith(QLatin1Char('+'));
            if (add) c.remove(0, 1);
            headerSortClick(m, v, c.toInt(), add);
            waitIdleForTest();
            QStringList keys, marks;
            for (const auto& k : m->sortKeys())
                keys << m->columnName(k.col) + (k.order == Qt::DescendingOrder ? QStringLiteral(":desc")
                                                                               : QStringLiteral(":asc"));
            for (int col = 0; col < m->columnCount(); ++col) {
                const QString mk = m->headerData(col, Qt::Horizontal, ArrowTableModel::SortMarkRole).toString();
                if (!mk.isEmpty()) marks << m->columnName(col) + QLatin1Char('=') + mk;
            }
            ArrowTableModel::SyncReads sync(m);
            std::printf("headersort %s keys=%s marks=%s indicator=%d first=%s\n", c.toUtf8().constData(),
                        keys.join(QLatin1Char(',')).toUtf8().constData(),
                        marks.join(QLatin1Char(',')).toUtf8().constData(),
                        v->horizontalHeader()->isSortIndicatorShown() ? 1 : 0,
                        m->data(m->index(0, 3), Qt::DisplayRole).toString().toUtf8().constData());
        }
    }
    // Self-test: select rows r0..r1 × columns c0..c1 of the active tab.
    bool selectForTest(int r0, int c0, int r1, int c1) {
        waitIdleForTest();
        auto* v = activeView();
        if (!v) return false;
        QItemSelection sel(v->model()->index(r0, c0), v->model()->index(r1, c1));
        v->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
        return true;
    }
    // Self-test: Copy As `fmt` (tsv, tsvh, csv, md, rows) of the selection.
    QString copyAsForTest(const QString& fmt) {
        if (fmt == QLatin1String("rows")) copyRows();
        else copySelectionAs(fmt == QLatin1String("csv") ? CopyFormat::Csv
                             : fmt == QLatin1String("md") ? CopyFormat::Markdown
                             : fmt == QLatin1String("tsvh") ? CopyFormat::TsvHeader : CopyFormat::Tsv,
                             false);
        return QApplication::clipboard()->text();
    }
    // Self-test: Export Selection to `out`; "" or the error, and the rows.
    QString exportSelectionForTest(const QString& out, qint64* rows) {
        VvCommandSpec s;
        if (!specForActiveTab(&s) || !restrictToSelection(&s)) return QStringLiteral("no selection");
        ExportFormat f;
        if (!exportFormatForPath(out, &f)) return QStringLiteral("unknown extension");
        startExport(s, out, f);
        while (exportWatcher_) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        *rows = lastExportRows_;
        return lastExportError_;
    }
    // Self-test: whether File ▸ Open URL is enabled, and the schemes.
    QString openUrlStateForTest() const {
        return QStringLiteral("enabled=%1 schemes=%2").arg(openUrlAct_->isEnabled() ? 1 : 0)
            .arg(QString::fromStdString(remote_schemes()));
    }
    // Self-test: apply each filter of `exprs` through the Filter box, save
    // the last as `saveAs` (unless empty); then the family's lists.
    QString filterHistoryForTest(const QStringList& exprs, const QString& saveAs) {
        waitIdleForTest();
        auto* m = activeModel();
        for (const QString& e : exprs) {
            useFilter(e);
            for (int g = 0; m && m->isComputing() && g < 2000000; ++g) QCoreApplication::processEvents();
        }
        if (!saveAs.isEmpty()) saveFilter(saveAs, filterEdit_->text().trimmed());
        const QString key = filterFamilyKey();
        QStringList saved;
        const QVariantMap sm = savedFilters_.value(key);
        for (auto it = sm.cbegin(); it != sm.cend(); ++it) saved << it.key() + QLatin1Char('=') + it.value().toString();
        return QStringLiteral("key=%1 recent=%2 saved=%3 completions=%4")
            .arg(key, recentFilters_.value(key).join(QLatin1Char('|')), saved.join(QLatin1Char('|')))
            .arg(filterHistModel_->rowCount());
    }
    // Self-test of Reload on the active tab's (local, text) file: apply
    // "filter;COL[:desc];HIDDEN", append `line` to the file, wait for the
    // changed-on-disk bar, reload; the rows and view state before and after.
    QString reloadForTest(const QString& spec, const QString& line) {
        waitIdleForTest();
        const QStringList p = spec.split(QLatin1Char(';'));
        auto* m = activeModel();
        auto* v = activeView();
        const OpenFile* f = activeFile();
        if (!m || !v || !f || p.size() != 3) return QStringLiteral("no table");
        const int id = f->id;
        useFilter(p[0]);
        sortActiveByNames({p[1]});
        hideActiveColumnForTest(p[2]);
        waitIdleForTest();
        auto state = [this](ArrowTableModel* mm) {
            const TabViewState st = captureViewState(tabs_->currentIndex());
            QStringList keys;
            for (const auto& [c, d] : st.sort) keys << c + (d ? QStringLiteral(":desc") : QStringLiteral(":asc"));
            return QStringLiteral("rows=%1 filter=%2 sort=%3 hidden=%4")
                .arg(mm->viewRows()).arg(st.filter, keys.join(QLatin1Char(',')), st.hidden.join(QLatin1Char(',')));
        };
        QString out = QStringLiteral("before ") + state(m);
        {
            QFile file(f->path);
            if (!file.open(QIODevice::Append)) return QStringLiteral("cannot append");
            file.write((line + QLatin1Char('\n')).toUtf8());
        }
        QElapsedTimer t;
        t.start();
        while (!contextBar_->changedShownForTest() && t.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        out += QStringLiteral(" | bar=%1").arg(contextBar_->changedShownForTest() ? 1 : 0);
        reloadFile(id);
        waitIdleForTest();
        auto* m2 = activeModel();
        for (int g = 0; m2 && m2->isComputing() && g < 2000000; ++g) QCoreApplication::processEvents();
        out += QStringLiteral(" | after ") + (m2 ? state(m2) : QStringLiteral("no table")) +
               QStringLiteral(" bar=%1").arg(contextBar_->changedShownForTest() ? 1 : 0);
        return out;
    }
    bool restoresSession() const { return restoreSession_; }
    // Self-test: each tab's label and view state, one line each.
    void dumpViewStatesForTest() {
        waitIdleForTest();
        for (int i = 0; i < tabs_->count(); ++i) materializeTab(i, false);
        waitIdleForTest();
        for (int i = 0; i < tabs_->count(); ++i) {
            if (auto* m = i < (int)models_.size() ? models_[(size_t)i] : nullptr)
                for (int g = 0; m->isComputing() && g < 2000000; ++g) QCoreApplication::processEvents();
            const TabViewState st = captureViewState(i);
            QStringList keys;
            for (const auto& [c, d] : st.sort) keys << c + (d ? QStringLiteral(":desc") : QStringLiteral(":asc"));
            std::printf("state %s%s filter=%s sort=%s hidden=%s cursor=%s:%lld frozen=%d\n",
                        i == tabs_->currentIndex() ? "*" : "", st.label.toUtf8().constData(),
                        st.filter.toUtf8().constData(), keys.join(QLatin1Char(',')).toUtf8().constData(),
                        st.hidden.join(QLatin1Char(',')).toUtf8().constData(),
                        st.curColumn.toUtf8().constData(), (long long)st.curRow, st.frozen);
        }
    }
    void saveSessionForTest() { waitIdleForTest(); saveSession(); }
    // Self-test of frozen columns: shown `width` px wide, freeze `n` columns
    // of the active tab and scroll right to the end; the overlay's columns,
    // whether it sits at the table's left edge with their width, and whether
    // putting the cursor in a frozen cell scrolls the table.
    QString freezeForTest(int n, int width) {
        waitIdleForTest();
        auto* v = dynamic_cast<TableView*>(activeView());
        auto* m = activeModel();
        if (!v || !m) return QStringLiteral("no table");
        resize(width, 500);
        show();
        auto pump = [](int ms) {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        };
        pump(200);
        v->setFrozenColumns(n);
        pump(100);
        QScrollBar* hs = v->horizontalScrollBar();
        hs->setValue(hs->maximum());
        pump(100);
        QTableView* o = v->frozenView();
        QStringList names;
        int w = 0;
        for (int c : v->frozenLogical()) { names << m->columnName(c); w += v->columnWidth(c); }
        const bool atLeft = o->x() == v->verticalHeader()->width() + v->frameWidth();
        const int before = hs->value();
        v->selectionModel()->setCurrentIndex(m->index(3, v->frozenLogical().value(0)),
                                             QItemSelectionModel::ClearAndSelect);
        pump(100);
        return QStringLiteral("frozen=%1 visible=%2 at-left=%3 width-ok=%4 scrolled=%5 kept=%6")
            .arg(names.join(QLatin1Char(','))).arg(o->isVisible() ? 1 : 0).arg(atLeft ? 1 : 0)
            .arg(o->width() == std::min(w, v->viewport()->width()) ? 1 : 0)
            .arg(before > 0 ? 1 : 0).arg(hs->value() == before ? 1 : 0);
    }
    // Self-test: the active tab's columns drawn as bases or qualities, and
    // the colours of the first characters of row 0 in the first of each.
    QString seqRenderForTest() {
        waitIdleForTest();
        auto* v = activeView();
        auto* m = activeModel();
        if (!v || !m) return QStringLiteral("no table");
        QStringList out;
        ArrowTableModel::SyncReads sync(m);
        for (int c = 0; c < m->columnCount(); ++c) {
            auto* sd = dynamic_cast<SequenceDelegate*>(v->itemDelegateForColumn(c));
            if (!sd) continue;
            const QString text = m->index(0, c).data(Qt::DisplayRole).toString().left(4);
            QStringList cols;
            for (QChar ch : text)
                cols << (sd->kind() == SequenceDelegate::Kind::Bases
                             ? SequenceDelegate::baseColor(ch, false).name()
                             : SequenceDelegate::qualityColor(ch.unicode() - 33).name());
            out << QStringLiteral("%1:%2[%3=%4]").arg(m->columnName(c),
                       sd->kind() == SequenceDelegate::Kind::Bases ? QStringLiteral("bases") : QStringLiteral("qual"),
                       text, cols.join(QLatin1Char(',')));
        }
        return out.isEmpty() ? QStringLiteral("none") : out.join(QLatin1Char(' '));
    }
    // Self-test: gg's arguments for row `row` of the active tab (gg need not
    // be installed), and whether gg was found.
    QString ggCommandForTest(int row) {
        waitIdleForTest();
        auto* m = activeModel();
        const OpenFile* f = activeFile();
        if (!m || !f) return QStringLiteral("no table");
        if (ggRole(f->feat.format) == GgRole::None) return QStringLiteral("not for gg");
        QStringList a = ggArguments(*f, row >= 0 ? rowLocus(m, row, f->feat.format) : QString());
        a.last() = QFileInfo(a.last()).fileName();
        return QStringLiteral("gg %1").arg(a.join(QLatin1Char(' ')));
    }
    // Self-test: the palette's rows for `query`, in order.
    void dumpPaletteForTest(const QString& query) {
        ActionPalette* p = openPalette(query);
        for (const QString& n : p->shown()) std::printf("palette %s\n", n.toUtf8().constData());
        p->close();
    }
    // Self-test: the Keyboard Shortcuts rows and the Field Reference sections.
    void dumpHelpForTest() {
        auto* d = ShortcutsDialog::showFor(this, commandsForHelp());
        std::printf("shortcuts rows=%d\n", d->rowCount());
        for (int r = 0; r < d->rowCount(); ++r) std::printf("shortcut-row %s\n", d->rowText(r).toUtf8().constData());
        d->close();
        std::printf("reference sections=%s\n", HelpDialog::sections().join(QLatin1Char(',')).toUtf8().constData());
        std::printf("reference anchor QUAL=%s obsp=%s AF_nfe=%s\n",
                    HelpDialog::anchorFor(QStringLiteral("QUAL")).toUtf8().constData(),
                    HelpDialog::anchorFor(QStringLiteral("obsp")).toUtf8().constData(),
                    HelpDialog::anchorFor(QStringLiteral("AF_nfe")).toUtf8().constData());
    }
    // Self-test: each tab's text and file group (files numbered in tab
    // order), then "groups=N" and the window title.
    void dumpTabLabelsForTest() {
        waitIdleForTest();
        QHash<int, int> order;
        for (int i = 0; i < tabs_->count(); ++i) {
            const int id = tabFile_.value(tabs_->widget(i), 0);
            if (id && !order.contains(id)) order.insert(id, order.size() + 1);
            std::printf("tab %s|%d\n", tabs_->tabText(i).toUtf8().constData(), order.value(id, 0));
        }
        std::printf("groups=%d\n", (int)order.size());
        std::printf("title=%s\n", windowTitle().toUtf8().constData());
        std::printf("strip=%s\n", contextBar_ && contextBar_->isVisibleTo(tabs_)
                                       ? contextBar_->fileForTest().toUtf8().constData() : "");
    }
    // Self-test: the Open dialog's types, one per line.
    static void dumpOpenFiltersForTest() {
        const QStringList filters = openNameFilters();
        std::printf("filters=%d\n", (int)filters.size());
        for (const QString& f : filters) std::printf("filter %s\n", f.toUtf8().constData());
    }
    // Self-test: toggle View ▸ Column Types and time the slot.
    qint64 toggleTypesForTest() {
        waitIdleForTest();
        QElapsedTimer t;
        t.start();
        typesAct_->toggle();
        return t.elapsed();
    }
};

// Deterministic, font-free self-check of the shared column-width planner. Run
// from VVG_SELFTEST so a regression in plan_column_widths fails CI. Returns true
// on success.
static bool checkWidthPlan() {
    auto eq = [](const std::vector<int>& got, std::vector<int> want,
                 const char* name) {
        if (got != want) {
            std::fprintf(stderr, "widthplan FAIL %s: got", name);
            for (int x : got) std::fprintf(stderr, " %d", x);
            std::fprintf(stderr, ", want");
            for (int x : want) std::fprintf(stderr, " %d", x);
            std::fprintf(stderr, "\n");
            return false;
        }
        return true;
    };
    bool ok = true;
    {   // p95 excludes a 5% long tail (95x20 + 5x100 -> 20, not the max, not the cap).
        std::vector<int> col; col.assign(95, 20); col.insert(col.end(), 5, 100);
        ok &= eq(plan_column_widths({col}, {}, 0, WidthPlanOptions{}), {20}, "tail");
    }
    {   // A wide/prose column is capped at c_max (50).
        std::vector<int> col; for (int v = 30; v < 130; ++v) col.push_back(v);
        ok &= eq(plan_column_widths({col}, {}, 0, WidthPlanOptions{}), {50}, "cap");
    }
    {   // Over budget: max-min fair water-filling truncates the widest equally.
        WidthPlanOptions o; o.c_max = 200;
        ok &= eq(plan_column_widths({{40},{60}}, {4,4}, 60, o), {30,30}, "waterfill");
    }
    {   // Fits within the viewport: everyone keeps their desired width.
        ok &= eq(plan_column_widths({{10},{10}}, {4,4}, 100, WidthPlanOptions{}),
                 {10,10}, "fits");
    }
    {   // Even the floors overflow: fall back to floors (scroll), never below.
        ok &= eq(plan_column_widths({{40},{40}}, {30,30}, 40, WidthPlanOptions{}),
                 {30,30}, "floors");
    }
    return ok;
}

// Deterministic check of the Copy-as-vv-command quoting and assembly. Run from
// VVG_SELFTEST. Returns true on success.
static bool checkVvCommand() {
    bool ok = true;
    auto eq = [&](const QString& got, const char* want) {
        if (got != QString::fromUtf8(want)) {
            std::fprintf(stderr, "vvcommand FAIL: got [%s], want [%s]\n",
                         got.toUtf8().constData(), want);
            ok = false;
        }
    };
    eq(shellQuote(QStringLiteral("/data/x.bam")), "/data/x.bam");
    eq(shellQuote(QStringLiteral("")), "''");
    eq(shellQuote(QStringLiteral("a b")), "'a b'");
    eq(shellQuote(QStringLiteral("it's")), "'it'\\''s'");
    eq(shellQuote(QStringLiteral("$HOME*")), "'$HOME*'");
    VvCommandSpec s;
    s.path = QStringLiteral("/d/my file.vcf.gz");
    eq(vvCommandLine(s), "vv '/d/my file.vcf.gz'");
    s.region = QStringLiteral("chr1:100-200");
    s.ncbi = true; s.slop = 50; s.gtStats = true;
    s.filter = QStringLiteral("AF > 0.05 and FILTER == \"PASS\"");
    s.sort = {{QStringLiteral("POS"), true}};
    s.select = QStringList{QStringLiteral("CHROM"), QStringLiteral("POS"), QStringLiteral("AF")};
    eq(vvCommandLine(s), "vv -r chr1:100-200 --coords NCBI --slop 50 --gt-stats "
                         "--filter 'AF > 0.05 and FILTER == \"PASS\"' --sort POS:desc "
                         "--select CHROM,POS,AF '/d/my file.vcf.gz'");
    VvCommandSpec t;
    t.path = QStringLiteral("a.h5ad"); t.tab = QStringLiteral("obs");
    t.sort = {{QStringLiteral("t:0"), false}};
    eq(vvCommandLine(t), "vv --tab obs --sort t:0:asc a.h5ad");
    t.sort = {{QStringLiteral("cell_type"), false}, {QStringLiteral("n_genes"), true}};
    eq(vvCommandLine(t), "vv --tab obs --sort cell_type --sort n_genes:desc a.h5ad");
    {
        const Config c = vvCommandConfig(t);
        eq(QString::fromStdString(c.sort_col + "|" + c.sort_then.at(0).col) +
               (c.sort_then.at(0).desc ? "|desc" : "|asc"), "cell_type|n_genes|desc");
    }
    t.contigs = true; t.region = QStringLiteral("chr1"); t.sort.clear();
    t.expand = QStringLiteral("INFO");      // --contigs takes no --expand
    eq(vvCommandLine(t), "vv --tab obs --contigs a.h5ad");
    VvCommandSpec g;
    g.path = QStringLiteral("genes.gff3.gz"); g.expand = QStringLiteral("attributes");
    g.filter = QStringLiteral("gene_type == \"lncRNA\"");
    eq(vvCommandLine(g), "vv --expand attributes --filter 'gene_type == \"lncRNA\"' genes.gff3.gz");
    // Layouts: only a non-default one is written.
    VvCommandSpec l;
    l.path = QStringLiteral("c.vcf"); l.samples = QStringLiteral("struct"); l.matrix = QStringLiteral("wide");
    eq(vvCommandLine(l), "vv c.vcf");
    l.samples = QStringLiteral("long");
    eq(vvCommandLine(l), "vv --samples long c.vcf");
    l.path = QStringLiteral("a.h5ad"); l.samples.clear(); l.matrix = QStringLiteral("long");
    eq(vvCommandLine(l), "vv --matrix long a.h5ad");
    if (vvCommandConfig(l).matrix != "long") { std::fprintf(stderr, "vvcommand FAIL: config matrix\n"); ok = false; }
    return ok;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Identity + icon. On Wayland the compositor picks the taskbar/dock icon by
    // matching the window's app_id to a .desktop file, so it must equal the
    // installed org.vv.Viewer.desktop base name — without this the window shows
    // a generic placeholder. setWindowIcon covers the title bar / X11 / an
    // uninstalled run, preferring the themed icon and falling back to the copy
    // embedded via vvg.qrc.
    app.setApplicationName(QStringLiteral("vv"));
    app.setApplicationDisplayName(QStringLiteral("vv"));
    app.setDesktopFileName(QStringLiteral("org.vv.Viewer"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("vv"),
                                       QIcon(QStringLiteral(":/icons/vv.svg"))));

    // Files, and vv's view flags (applied once the files are open).
    QStringList paths;
    MainWindow::ViewOptions view;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](QString* out) {
            if (i + 1 >= argc) { std::fprintf(stderr, "vvg: %s needs a value\n", a.c_str()); std::exit(2); }
            *out = QString::fromLocal8Bit(argv[++i]);
        };
        QString tmp;
        if      (a == "--filter")                 value(&view.filter);
        else if (a == "--select")                 value(&view.select);
        else if (a == "--sort")                   { value(&tmp); view.sort << tmp; }
        else if (a == "--tab")                    value(&view.tab);
        else if (a == "-r" || a == "--region")    value(&view.region);
        else if (a == "--tags")                   value(&view.tags);
        else if (a == "--samples") {
            value(&view.samples);
            if (view.samples != "struct" && view.samples != "long" && view.samples != "text") {
                std::fprintf(stderr, "vvg: --samples: unknown layout '%s' (use struct|long|text)\n",
                             view.samples.toLocal8Bit().constData());
                return 2;
            }
        }
        else if (a == "--matrix") {
            value(&view.matrix);
            if (view.matrix != "wide" && view.matrix != "long") {
                std::fprintf(stderr, "vvg: --matrix: unknown layout '%s' (use wide|long)\n",
                             view.matrix.toLocal8Bit().constData());
                return 2;
            }
        }
        else if (a == "--coords")                 { value(&tmp); view.ncbi = tmp.compare(QStringLiteral("NCBI"), Qt::CaseInsensitive) == 0; }
        else if (a == "--slop")                   { value(&tmp); view.slop = tmp.toInt(); }
        else if (a == "--pileup")                 view.pileup = true;
        else if (a == "--gt-stats")               view.gtStats = true;
        else if (a == "--contigs")                view.contigs = true;
        else if (a == "-h" || a == "--help") {
            std::printf("usage: vvg [options] [file ...]\n"
                        "  --filter EXPR     keep rows matching EXPR (vv's --filter grammar)\n"
                        "  --select TERMS    show these columns, in this order (vv's --select)\n"
                        "  --sort COL[:desc] sort by a column\n"
                        "  --tab NAME        open a component tab (or its 0-based index)\n"
                        "  -r, --region R    region query (UCSC coordinates; --coords NCBI)\n"
                        "  --samples struct|long|text   VCF/BCF sample columns (as in vv)\n"
                        "  --matrix wide|long           HDF5 / AnnData matrix tabs (as in vv)\n"
                        "  --slop N, --tags LIST, --pileup, --gt-stats, --contigs\n"
                        "                    as in vv\n");
            return 0;
        }
        else if (!a.empty() && a[0] == '-' && a != "-") {
            std::fprintf(stderr, "vvg: unknown option '%s' (vvg --help)\n", a.c_str());
            return 2;
        }
        else paths << QString::fromStdString(a);
    }

    if (const char* st = std::getenv("VVG_SELFTEST"); st && *st && *st != '0') {
        if (!checkWidthPlan()) return 1;
        if (!checkVvCommand()) return 1;
        // App identity: the desktop file name must match the installed .desktop
        // (Wayland taskbar icon), and the window icon must resolve — the themed
        // "vv" or, uninstalled, the copy embedded via vvg.qrc.
        if (app.desktopFileName() != QStringLiteral("org.vv.Viewer")) {
            std::fprintf(stderr, "vvg: desktop file name not set\n"); return 1;
        }
        if (app.windowIcon().isNull()) {
            std::fprintf(stderr, "vvg: window icon did not resolve\n"); return 1;
        }
        std::printf("widthplan=ok vvcommand=ok icon=ok\n");
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
            // Exercise the typed sort path on column 0. Emit the first row for
            // both directions: a working sort makes them differ (unless column 0
            // is constant), so CI can assert the rows actually reorder rather
            // than the header indicator just toggling.
            auto row0 = [&]() {
                std::string s;
                for (int c = 0; c < m.columnCount(); ++c) {
                    if (c) s += " | ";
                    s += m.data(m.index(0, c), Qt::DisplayRole).toString().toStdString();
                }
                return s;
            };
            m.sortByDisplayColumn(0, Qt::AscendingOrder);
            std::printf("sorted_row0=%s\n", row0().c_str());
            m.sortByDisplayColumn(0, Qt::DescendingOrder);
            std::printf("sorted_row0_desc=%s\n", row0().c_str());
            m.sortByDisplayColumn(-1, Qt::AscendingOrder);   // reset
        }
        // Clipboard copy uses RawTextRole (the value without digit grouping),
        // so a grouped integer like "11_200" is copied as "11200". Verify the
        // raw role differs from the display role on any grouped cell, and that a
        // full row's raw TSV has one field per column.
        {
            int grouped = 0, mismatch = 0;
            for (int r = 0; r < m.rowCount() && r < 200; ++r)
                for (int c = 0; c < m.columnCount(); ++c) {
                    QString disp = m.index(r, c).data(Qt::DisplayRole).toString();
                    QString raw  = m.index(r, c).data(ArrowTableModel::RawTextRole).toString();
                    if (disp.contains(QLatin1Char('_')) && !disp.contains(QLatin1Char(' '))) {
                        ++grouped;
                        QString ungrouped = disp; ungrouped.remove(QLatin1Char('_'));
                        if (raw == ungrouped) ++mismatch;
                    }
                }
            std::printf("copy_raw_grouped=%d copy_raw_ungrouped=%d\n", grouped, mismatch);
            if (grouped > 0 && mismatch != grouped) {
                std::fprintf(stderr, "copy: RawTextRole failed to ungroup "
                             "%d of %d cells\n", grouped - mismatch, grouped);
                return 1;
            }
            if (m.columnCount() > 0) {
                QString rowtsv;
                for (int c = 0; c < m.columnCount(); ++c) {
                    if (c) rowtsv += QLatin1Char('\t');
                    rowtsv += m.index(0, c).data(ArrowTableModel::RawTextRole).toString();
                }
                std::printf("copy_row_fields=%lld\n",
                            (long long)(rowtsv.count(QLatin1Char('\t')) + 1));
            }
        }
        // Optional filter check: VVG_FILTER="<expr>".
        if (const char* fe = std::getenv("VVG_FILTER"); fe && *fe) {
            FilterExpr fx; std::string err;
            if (parse_filter_expr(fe, *m.schema(), &fx, &err)) {
                m.setFilter(fx);
                std::printf("filter '%s' -> %lld/%lld rows\n", fe,
                            (long long)m.viewRows(), (long long)m.sourceTotal());
                // Sorting within the filter keeps exactly the filtered rows.
                if (m.columnCount() > 0) {
                    m.sortByDisplayColumn(m.columnCount() - 1, Qt::DescendingOrder);
                    std::string r0;
                    for (int c = 0; c < m.columnCount(); ++c)
                        r0 += (c ? " | " : "") + m.data(m.index(0, c), Qt::DisplayRole).toString().toStdString();
                    std::printf("filter_sorted_desc rows=%lld row0=%s\n",
                                (long long)m.viewRows(), r0.c_str());
                    m.sortByDisplayColumn(-1, Qt::AscendingOrder);
                }
            } else {
                std::printf("filter parse error: %s\n", err.c_str());
            }
        }
        // Optional search check: VVG_FIND="<regex>".
        if (const char* se = std::getenv("VVG_FIND"); se && *se) {
            m.setSearch(QRegularExpression(se, QRegularExpression::CaseInsensitiveOption));
            QModelIndex hit = m.findNext(QModelIndex(), true);
            if (hit.isValid()) {
                // The hit is highlighted, row 0 / column 0 only if it matches.
                auto lit = [&](const QModelIndex& i) {
                    m.data(i, Qt::DisplayRole);
                    return m.data(i, Qt::BackgroundRole).isValid() ? 1 : 0;
                };
                std::printf("find '%s' -> row %d col %d = %s highlight=%d first=%d\n", se,
                            hit.row(), hit.column(),
                            m.data(hit, Qt::DisplayRole).toString().toStdString().c_str(),
                            lit(hit), lit(m.index(0, 0)));
            } else
                std::printf("find '%s' -> no match\n", se);
        }
        return 0;
    }

    // Window-level offscreen smoke test: build the shell and open the file(s)
    // headlessly (quiet = no blocking modal dialogs). Exercises the
    // addSourceTab / loadSources path the menus and drag-and-drop also use.
    if (const char* wt = std::getenv("VVG_WINTEST"); wt && *wt && *wt != '0') {
        MainWindow win;
        win.setHeadless(true);
        if (const char* ex = std::getenv("VVG_EXPAND"); ex && *ex == '0')
            win.setExpandForTest(false);
        win.setJsonAsTable(!view.filter.isEmpty() || !view.select.isEmpty() ||
                           !view.sort.isEmpty() || !view.tab.isEmpty());
        win.openPaths(paths, /*quiet=*/true);
        for (const QString& e : win.applyViewOptions(view))
            std::fprintf(stderr, "vvg: %s\n", e.toLocal8Bit().constData());
        win.waitIdleForTest();
        std::printf("win_tabs=%d\n", win.tabCount());
        if (const char* sm = std::getenv("VVG_SCROLLMODE"); sm && *sm && *sm != '0')
            std::printf("scroll=%s\n", win.scrollModeForTest());
        // Optional shortcut dump: VVG_SHORTCUTS=1 lists each key and its
        // action(s), then shortcut_conflicts=N.
        if (const char* sk = std::getenv("VVG_SHORTCUTS"); sk && *sk && *sk != '0')
            win.dumpShortcutsForTest();
        // Optional menu dump: VVG_MENU_DUMP=1 (the menu bar and ≡ trees).
        if (const char* md = std::getenv("VVG_MENU_DUMP"); md && *md && *md != '0')
            win.dumpMenusForTest();
        // Optional stream check: VVG_STREAM_TEST=1 (streamForTest), with
        // VVG_STREAM_BUDGET=<bytes> read at start.
        if (const char* sr = std::getenv("VVG_STREAM_TEST"); sr && *sr && *sr != '0')
            std::printf("stream %s\n", win.streamForTest().toStdString().c_str());
        // Optional large-file question check: VVG_FULLPASS="<rows>,<bytes>,yes|no"
        // (fullPassForTest).
        if (const char* fp = std::getenv("VVG_FULLPASS"); fp && *fp) {
            const QStringList p = QString::fromLocal8Bit(fp).split(QLatin1Char(','));
            std::printf("fullpass %s\n", win.fullPassForTest(p.value(0).toLongLong(), p.value(1).toLongLong(),
                                                              p.value(2) == QLatin1String("yes"))
                                              .toStdString().c_str());
        }
        // Optional cancel check: VVG_CANCEL_TEST=<ms> (cancelForTest).
        if (const char* ct = std::getenv("VVG_CANCEL_TEST"); ct && *ct)
            std::printf("cancel %s\n", win.cancelForTest(std::atoi(ct)).toStdString().c_str());
        // Optional vertical jump check: VVG_GOTO="<0-based row>[,<frozen columns>]".
        if (const char* gt = std::getenv("VVG_GOTO"); gt && *gt) {
            const QStringList p = QString::fromLocal8Bit(gt).split(QLatin1Char(','));
            std::printf("goto %s\n", win.gotoForTest(p.value(0).toInt(), p.value(1).toInt())
                                          .toStdString().c_str());
        }
        if (const char* hs = std::getenv("VVG_HSCROLL"); hs && *hs)
            {
                auto [val, max] = win.hscrollForTest(std::atoi(hs));
                std::printf("hscroll=%d max=%d\n", val, max);
            }
        // Optional per-tab dimension dump: VVG_TABDIMS=1 (verifies dense 2-D
        // matrix previews are capped, not fully densified).
        if (const char* td = std::getenv("VVG_TABDIMS"); td && *td && *td != '0')
            win.dumpTabDims();
        // Optional tab close before the re-open check: VVG_CLOSE=<tab index>
        // (a closed file must stay closed when another file re-opens).
        if (const char* cl = std::getenv("VVG_CLOSE"); cl && *cl)
            win.closeTabForTest(std::atoi(cl));
        // Optional region/pileup re-open check: VVG_REGION="chr1:…" [VVG_NCBI=1]
        // [VVG_PILEUP=1]. It re-opens the active tab's file only;
        // VVG_TABDIMS_AFTER=1 then prints every tab.
        const char* rg = std::getenv("VVG_REGION");
        bool pileup = std::getenv("VVG_PILEUP") != nullptr;
        const char* tg = std::getenv("VVG_TAGS");
        bool gtStats = std::getenv("VVG_GTSTATS") != nullptr;
        bool contigs = std::getenv("VVG_CONTIGS") != nullptr;
        if ((rg && *rg) || pileup || (tg && *tg) || gtStats || contigs) {
            win.applyRegionQuery(QString::fromLocal8Bit(rg ? rg : ""),
                                 std::getenv("VVG_NCBI") != nullptr, 0, pileup,
                                 QString::fromLocal8Bit(tg ? tg : ""), gtStats, contigs);
            win.waitIdleForTest();
            std::printf("region '%s' pileup=%d tags='%s' gtstats=%d contigs=%d "
                        "-> tabs=%d rows=%d cols=%d\n",
                        rg ? rg : "", pileup ? 1 : 0, tg ? tg : "", gtStats ? 1 : 0,
                        contigs ? 1 : 0,
                        win.tabCount(), win.firstTabRows(), win.firstTabCols());
            if (const char* ta = std::getenv("VVG_TABDIMS_AFTER"); ta && *ta && *ta != '0')
                win.dumpTabDims();
        }
        // Optional async-filter check: VVG_AFILTER="<expr>" (off-thread + pump).
        if (const char* af = std::getenv("VVG_AFILTER"); af && *af) {
            int rows = win.applyFilterAsyncForTest(QString::fromLocal8Bit(af));
            std::printf("afilter '%s' -> rows=%d\n", af, rows);
        }
        // Optional async-find check: VVG_AFIND="<regex>" (off-thread + pump).
        if (const char* fa = std::getenv("VVG_AFIND"); fa && *fa) {
            qint64 n = win.findAsyncForTest(QString::fromLocal8Bit(fa));
            std::printf("afind '%s' -> matches=%lld\n", fa, (long long)n);
        }
        // Optional tab / sort / hidden-column setup and the Copy-as-vv-command
        // result: VVG_TAB=<index>, VVG_SORT="COL[:desc]", VVG_HIDE="A,B",
        // VVG_COPYCMD=1 prints "cmd=<command line>".
        if (const char* ti = std::getenv("VVG_TAB"); ti && *ti)
            win.selectTabForTest(std::atoi(ti));
        // Optional context-bar dump: VVG_CONTEXT=1 prints the active tab's
        // format options, e.g. "context: region pileup tags contigs".
        if (const char* cx = std::getenv("VVG_CONTEXT"); cx && *cx && *cx != '0')
            std::printf("context: %s\n", win.contextForTest().toUtf8().constData());
        // Optional header check: VVG_HEADER=1 (the first column's header).
        if (const char* hd = std::getenv("VVG_HEADER"); hd && *hd && *hd != '0')
            std::printf("header %s\n", win.headerForTest().toUtf8().constData());
        // Optional cell-filter check: VVG_CELLFILTER="row,col,…" (Filter to)
        // or VVG_CELLEXCLUDE (Exclude) on the active tab.
        if (const char* cf = std::getenv("VVG_CELLFILTER"); cf && *cf)
            std::printf("cellfilter %s\n", win.cellFilterForTest(QString::fromLocal8Bit(cf), false).toUtf8().constData());
        if (const char* ce = std::getenv("VVG_CELLEXCLUDE"); ce && *ce)
            std::printf("cellexclude %s\n", win.cellFilterForTest(QString::fromLocal8Bit(ce), true).toUtf8().constData());
        // Optional sort-click check: VVG_HEADERSORT="3,+0,…" (headerSortForTest).
        if (const char* hs = std::getenv("VVG_HEADERSORT"); hs && *hs)
            win.headerSortForTest(QString::fromLocal8Bit(hs));
        // Optional Open URL check: VVG_OPENURL_STATE=1.
        if (const char* ou = std::getenv("VVG_OPENURL_STATE"); ou && *ou && *ou != '0')
            std::printf("openurl %s\n", win.openUrlStateForTest().toUtf8().constData());
        // Optional filter-history check: VVG_FILTERHIST="expr;expr…" applies
        // each through the Filter box, VVG_SAVEFILTER=<name> saves the last.
        if (const char* fh = std::getenv("VVG_FILTERHIST"); fh) {
            const char* sv = std::getenv("VVG_SAVEFILTER");
            std::printf("filterhist %s\n", win.filterHistoryForTest(
                QString::fromLocal8Bit(fh).split(QLatin1Char(';'), Qt::SkipEmptyParts),
                sv ? QString::fromLocal8Bit(sv) : QString()).toUtf8().constData());
        }
        // Optional reload check: VVG_RELOAD_TEST="filter;COL[:desc];HIDDEN"
        // with VVG_RELOAD_APPEND=<line> (reloadForTest).
        if (const char* rt = std::getenv("VVG_RELOAD_TEST"); rt && *rt) {
            const char* ap = std::getenv("VVG_RELOAD_APPEND");
            std::printf("reload %s\n", win.reloadForTest(QString::fromLocal8Bit(rt),
                                                         ap ? QString::fromLocal8Bit(ap) : QString())
                                           .toUtf8().constData());
        }
        // Optional palette check: VVG_PALETTE="<query>" (the commands it lists).
        if (const char* pq = std::getenv("VVG_PALETTE"); pq)
            win.dumpPaletteForTest(QString::fromLocal8Bit(pq));
        // Optional help check: VVG_HELP=1 (dumpHelpForTest).
        if (const char* hp = std::getenv("VVG_HELP"); hp && *hp && *hp != '0')
            win.dumpHelpForTest();
        // Optional tab-label check: VVG_TABLABELS=1 (dumpTabLabelsForTest).
        if (const char* tl = std::getenv("VVG_TABLABELS"); tl && *tl && *tl != '0')
            win.dumpTabLabelsForTest();
        // Optional width check: VVG_COLWIDTHS=1 (the active tab's columns).
        if (const char* cw = std::getenv("VVG_COLWIDTHS"); cw && *cw && *cw != '0')
            std::printf("colwidths %s\n", win.colWidthsForTest().toUtf8().constData());
        // Optional Open-dialog check: VVG_FILTERS=1 prints its file types.
        if (const char* fl = std::getenv("VVG_FILTERS"); fl && *fl && *fl != '0')
            MainWindow::dumpOpenFiltersForTest();
        // Optional settings check: VVG_TOGGLE_TYPES=1 toggles View ▸ Column
        // Types, prints how long the action took and the header after it.
        if (const char* tt = std::getenv("VVG_TOGGLE_TYPES"); tt && *tt && *tt != '0') {
            const qint64 ms = win.toggleTypesForTest();
            std::printf("toggle_ms=%lld header %s\n", (long long)ms,
                        win.headerForTest().toUtf8().constData());
        }
        // Optional layout check: VVG_WIDTH=<px> shows the window that wide and
        // prints "width=<actual> overlaps=<n>" for the toolbar and the strip.
        if (const char* wd = std::getenv("VVG_WIDTH"); wd && *wd)
            std::printf("%s\n", win.overlapsForTest(std::atoi(wd)).toUtf8().constData());
        // Optional lazy-tab check: VVG_BUILT=1 prints "built=N/M", the tabs
        // whose model exists (has read the source) of all tabs.
        if (const char* bt = std::getenv("VVG_BUILT"); bt && *bt && *bt != '0')
            std::printf("built=%d/%d\n", win.builtTabsForTest(), win.tabCountForTest());
        // Optional asynchronous-read checks: VVG_CELLS="row:col,…" (cellsForTest),
        // VVG_CLOSE_LOADING=<tab> (closeLoadingForTest).
        if (const char* ce = std::getenv("VVG_CELLS"); ce && *ce)
            std::printf("cells %s\n", win.cellsForTest(QString::fromLocal8Bit(ce)).toUtf8().constData());
        if (const char* cl = std::getenv("VVG_CLOSE_LOADING"); cl && *cl)
            std::printf("close-loading %s\n", win.closeLoadingForTest(std::atoi(cl)).toUtf8().constData());
        // Optional superseded-jobs check: VVG_RACE="<filter>" (raceForTest).
        if (const char* rc = std::getenv("VVG_RACE"); rc && *rc)
            std::printf("race %s\n", win.raceForTest(QString::fromLocal8Bit(rc)).toUtf8().constData());
        // VVG_SORT="COL[:desc][,COL2[:desc]…]": sort keys, the first deciding.
        if (const char* so = std::getenv("VVG_SORT"); so && *so) {
            const QString bad = win.sortActiveByNames(QString::fromLocal8Bit(so).split(QLatin1Char(',')));
            if (!bad.isEmpty()) std::fprintf(stderr, "vvg: no column '%s' to sort\n", bad.toLocal8Bit().constData());
        }
        // Optional paint check: VVG_SETTLE=<ms> (after VVG_SORT): the shown
        // view must stop reading with every visible row loaded.
        if (const char* st = std::getenv("VVG_SETTLE"); st && *st)
            std::printf("settle %s\n", win.settleForTest(std::atoi(st)).toUtf8().constData());
        if (const char* hd = std::getenv("VVG_HIDE"); hd && *hd)
            for (const QString& c : QString::fromLocal8Bit(hd).split(QLatin1Char(',')))
                if (!win.hideActiveColumnForTest(c))
                    std::fprintf(stderr, "vvg: no column '%s' to hide\n", c.toLocal8Bit().constData());
        // Optional Genome Goggles check: VVG_GG_CMD=<row> (-1: the file).
        if (const char* gc = std::getenv("VVG_GG_CMD"); gc && *gc)
            std::printf("ggcmd %s\n", win.ggCommandForTest(std::atoi(gc)).toUtf8().constData());
        // Optional sequence-colour check: VVG_SEQRENDER=1.
        if (const char* sr = std::getenv("VVG_SEQRENDER"); sr && *sr && *sr != '0')
            std::printf("seqrender %s\n", win.seqRenderForTest().toUtf8().constData());
        // Optional frozen-column check: VVG_FREEZE="<n>,<window width>".
        if (const char* fz = std::getenv("VVG_FREEZE"); fz && *fz) {
            const QStringList q = QString::fromLocal8Bit(fz).split(QLatin1Char(','));
            std::printf("freeze %s\n", win.freezeForTest(q.value(0).toInt(), q.value(1, QStringLiteral("420")).toInt())
                                           .toUtf8().constData());
        }
        // Optional session checks: VVG_REOPEN_CLOSED=<tab> closes that tab
        // and reopens it; VVG_SESSION_SAVE=1 saves the session as closing the
        // window does; VVG_RESTORE=1 (with no files) restores it;
        // VVG_VIEWSTATES=1 prints each tab's view state.
        if (const char* rc = std::getenv("VVG_REOPEN_CLOSED"); rc && *rc) {
            win.closeTabForTest(std::atoi(rc));
            std::printf("reopen-closed %d\n", win.reopenClosedTab() ? 1 : 0);
        }
        if (const char* sr = std::getenv("VVG_RESTORE"); sr && *sr && *sr != '0' && paths.isEmpty())
            std::printf("restored files=%d\n", win.restoreSession());
        if (const char* vs = std::getenv("VVG_VIEWSTATES"); vs && *vs && *vs != '0')
            win.dumpViewStatesForTest();
        if (const char* ss = std::getenv("VVG_SESSION_SAVE"); ss && *ss && *ss != '0') {
            win.saveSessionForTest();
            std::printf("session saved\n");
        }
        // Optional copy / export-selection checks: VVG_SELECT="r0,c0,r1,c1"
        // selects a block (after VVG_SORT / VVG_HIDE), then VVG_COPYAS=<fmt>
        // prints the clipboard and VVG_EXPORT_SEL=<path> exports it.
        if (const char* se = std::getenv("VVG_SELECT"); se && *se) {
            const QStringList q = QString::fromLocal8Bit(se).split(QLatin1Char(','));
            if (q.size() == 4) win.selectForTest(q[0].toInt(), q[1].toInt(), q[2].toInt(), q[3].toInt());
            if (const char* ca = std::getenv("VVG_COPYAS"); ca && *ca)
                std::printf("copyas:\n%s\n", win.copyAsForTest(QString::fromLocal8Bit(ca)).toUtf8().constData());
            if (const char* es = std::getenv("VVG_EXPORT_SEL"); es && *es) {
                qint64 rows = 0;
                const QString err = win.exportSelectionForTest(QString::fromLocal8Bit(es), &rows);
                if (err.isEmpty()) std::printf("export-selection -> rows=%lld\n", (long long)rows);
                else std::printf("export-selection -> error: %s\n", err.toUtf8().constData());
            }
        }
        if (const char* ex = std::getenv("VVG_EXPORT"); ex && *ex) {
            qint64 rows = 0;
            const char* ec = std::getenv("VVG_EXPORT_CANCEL_AFTER");
            QString err = win.exportActiveForTest(QString::fromLocal8Bit(ex), &rows,
                                                  ec && *ec ? std::atoll(ec) : -1);
            if (err.isEmpty()) std::printf("export -> rows=%lld\n", (long long)rows);
            else               std::printf("export -> error: %s\n", err.toStdString().c_str());
        }
        if (const char* cc = std::getenv("VVG_COPYCMD"); cc && *cc && *cc != '0')
            std::printf("cmd=%s\n", win.commandForActiveTab().toLocal8Bit().constData());
        // Optional detail-dock check: VVG_DETAIL="r,c;r,c;…" moves the cursor
        // and prints "detail r,c -> row=… rows=… first=… reused=…" per step.
        if (const char* dt = std::getenv("VVG_DETAIL"); dt && *dt)
            for (const QString& step : QString::fromLocal8Bit(dt).split(QLatin1Char(';'))) {
                const QStringList rc = step.split(QLatin1Char(','));
                if (rc.size() != 2) continue;
                std::printf("detail %s -> %s\n", step.toLocal8Bit().constData(),
                            win.detailForTest(rc[0].toInt(), rc[1].toInt()).toLocal8Bit().constData());
            }
        // Optional Column tab: VVG_COLSTATS=<display column> prints
        // "colstats <statistic=value …> top=… scope=…".
        // VVG_STATS_AUTO_ROWS=N: summarise only on request above N rows
        // (default 10,000,000); VVG_COLSTATS_COMPUTE=<display column> then
        // clicks the Compute button and prints "colstats-compute …".
        if (const char* ar = std::getenv("VVG_STATS_AUTO_ROWS"); ar && *ar)
            win.setStatsAutoRowsForTest(std::atoll(ar));
        if (const char* cs = std::getenv("VVG_COLSTATS"); cs && *cs)
            std::printf("colstats %s\n", win.columnStatsForTest(std::atoi(cs)).toUtf8().constData());
        if (const char* cc = std::getenv("VVG_COLSTATS_COMPUTE"); cc && *cc)
            std::printf("colstats-compute %s\n",
                        win.computeColumnStatsForTest(std::atoi(cc)).toUtf8().constData());
        // Optional header click: VVG_HEADERCLICK=<display column> clicks
        // that column's header and prints "headerclick sort=… cursor=r,c
        // title=… where=… <Column tab>".
        if (const char* hc = std::getenv("VVG_HEADERCLICK"); hc && *hc)
            std::printf("headerclick %s\n", win.headerClickForTest(std::atoi(hc)).toUtf8().constData());
        // Optional Column-tab location: VVG_SHOWCOL=<display column> prints
        // "showcol before=… after=… x=… visible=…".
        if (const char* sc = std::getenv("VVG_SHOWCOL"); sc && *sc)
            std::printf("showcol %s\n", win.showColumnForTest(std::atoi(sc)).toUtf8().constData());
        // Optional selection summary: VVG_SELSUM="r0,c0,r1,c1" selects that
        // block and prints "selsum <status-bar text>".
        if (const char* ss = std::getenv("VVG_SELSUM"); ss && *ss) {
            const QStringList q = QString::fromLocal8Bit(ss).split(QLatin1Char(','));
            if (q.size() == 4)
                std::printf("selsum %s\n", win.selectionSummaryForTest(
                    q[0].toInt(), q[1].toInt(), q[2].toInt(), q[3].toInt()).toUtf8().constData());
        }
        // Optional JSON tree check: VVG_TREE=<jq path> selects it in the
        // active tree tab and prints it; VVG_TREE_TABLE=1 then opens the
        // records there as a table tab and prints its size.
        if (const char* tp = std::getenv("VVG_TREE"); tp && *tp)
            win.jsonTreeForTest(QString::fromLocal8Bit(tp), std::getenv("VVG_TREE_TABLE") != nullptr);
        // Optional tree find: VVG_TREE_FIND="q1;q2;…" runs Find (forward;
        // a query starting with "?" backward) from the selection, one after
        // another, printing where each lands.
        if (const char* tf = std::getenv("VVG_TREE_FIND"); tf && *tf)
            for (const QString& q : QString::fromLocal8Bit(tf).split(QLatin1Char(';')))
                win.jsonFindForTest(q.startsWith(QLatin1Char('?')) ? q.mid(1) : q, !q.startsWith(QLatin1Char('?')));
        // Optional screenshot for the docs: VVG_SCREENSHOT=out.png, after the
        // options above (tab, sort, detail row, ...); VVG_SCREENSHOT_SIZE=WxH
        // (default 1100x700). Works headless (QT_QPA_PLATFORM=offscreen).
        if (const char* sh = std::getenv("VVG_SCREENSHOT"); sh && *sh) {
            int w = 1100, h = 700;
            if (const char* sz = std::getenv("VVG_SCREENSHOT_SIZE"); sz && *sz)
                std::sscanf(sz, "%dx%d", &w, &h);
            win.resize(std::max(320, w), std::max(240, h));
            win.show();
            // Let layout and short animations (the filter field's clear
            // button fades in) finish, so the image is the same every run.
            QElapsedTimer settle;
            settle.start();
            while (settle.elapsed() < 400) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            win.waitIdleForTest();
            if (win.grab().save(QString::fromLocal8Bit(sh)))
                std::printf("screenshot -> %s\n", sh);
            else {
                std::fprintf(stderr, "vvg: cannot write %s\n", sh);
                return 1;
            }
        }
        return 0;
    }

    MainWindow win;
    win.show();
    if (!paths.isEmpty()) {
        win.setJsonAsTable(!view.filter.isEmpty() || !view.select.isEmpty() ||
                           !view.sort.isEmpty() || !view.tab.isEmpty());
        win.openPaths(paths);
        const QStringList errs = win.applyViewOptions(view);
        for (const QString& e : errs) std::fprintf(stderr, "vvg: %s\n", e.toLocal8Bit().constData());
        if (!errs.isEmpty()) win.statusBar()->showMessage(errs.join(QStringLiteral("; ")));
    } else {
        win.setWindowTitle(QStringLiteral("vv"));
        if (win.restoresSession()) win.restoreSession();
    }
    return app.exec();
}
