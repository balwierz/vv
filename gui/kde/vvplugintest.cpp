// Headless check of the KF6 part and Properties plugins (KF6 builds only):
//   vvplugintest --part  PLUGIN.so FILE   load the part as a host does, open
//                                         FILE, print its tabs and the first
//                                         one's rows / columns
//   vvplugintest --props PLUGIN.so FILE   build FILE's Properties dialog with
//                                         the plugin, show its Format page (its
//                                         sections), then its Table page, the same
//   VVPLUGINTEST_SORT=1                   then click the first header: whether
//                                         it sorted, and the pane's message
// Both first print the plugin's MIME type count (0 means KF6 will never
// offer it, as with a thumbnailer whose MimeTypes sat outside KPlugin).
#include <KFileItem>
#include <KPageWidgetModel>
#include <KParts/PartLoader>
#include <KParts/ReadOnlyPart>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <KPropertiesDialog>
#include <KPropertiesDialogPlugin>

#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QTabBar>
#include <QTableView>
#include <QTreeWidget>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QUrl>

#include <cstdio>


static void pump(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// The pane is found by class name and read through Qt's own classes: the
// plugin links its own copy of vvgui_core, so qobject_cast to VvTablePane
// (a different meta-object here) would fail.
static QWidget* findPane(QObject* root) {
    if (auto* w = qobject_cast<QWidget*>(root); w && qstrcmp(w->metaObject()->className(), "VvTablePane") == 0)
        return w;
    for (QWidget* w : root->findChildren<QWidget*>())
        if (qstrcmp(w->metaObject()->className(), "VvTablePane") == 0) return w;
    return nullptr;
}

// Select the dialog page whose widget is `w`.
static void selectPage(KPropertiesDialog& dlg, QWidget* w) {
    if (auto* pm = dlg.findChild<KPageWidgetModel*>())
        for (int r = 0; r < pm->rowCount(); ++r)
            if (KPageWidgetItem* item = pm->item(pm->index(r, 0)); item && item->widget() == w)
                dlg.setCurrentPage(item);
}

// The Format page: nothing read before it is shown, then its sections and
// (Parquet) whether a column has a "Physical type" row.
static int reportFormat(KPropertiesDialog& dlg) {
    QTreeWidget* tree = nullptr;
    for (QTreeWidget* t : dlg.findChildren<QTreeWidget*>())
        if (qstrcmp(t->metaObject()->className(), "DetailsTree") == 0) tree = t;
    if (!tree) { std::printf("no Format page\n"); return 1; }
    std::printf("format_before=%d\n", tree->topLevelItemCount());
    selectPage(dlg, tree);
    QElapsedTimer t;
    t.start();
    auto reading = [tree] {
        return tree->topLevelItemCount() == 0 ||
               tree->topLevelItem(0)->text(0) == QString::fromUtf8("Reading\u2026");
    };
    while (reading() && t.elapsed() < 10000) pump(50);
    QString secs;
    bool physical = false;
    for (int k = 0; k < tree->topLevelItemCount(); ++k) {
        QTreeWidgetItem* sec = tree->topLevelItem(k);
        secs += (k ? " " : "") + sec->text(0);
        if (sec->text(0) == QLatin1String("Columns"))
            for (int c = 0; c < sec->childCount(); ++c)
                for (int r = 0; r < sec->child(c)->childCount(); ++r)
                    physical |= sec->child(c)->child(r)->text(0) == QLatin1String("Physical type");
    }
    std::printf("page=%s\nformat=%s physical=%d\n",
                dlg.currentPage() ? dlg.currentPage()->name().toUtf8().constData() : "-",
                secs.toUtf8().constData(), physical ? 1 : 0);
    return reading() ? 1 : 0;
}

static int report(QWidget* pane) {
    if (!pane) { std::printf("no table pane\n"); return 1; }
    auto* bar = pane->findChild<QTabBar*>();
    const int n = bar ? bar->count() : 0;
    std::printf("tabs=%d", n);
    for (int i = 0; i < n; ++i) std::printf(" %s", bar->tabText(i).toUtf8().constData());
    std::printf("\n");
    QElapsedTimer t;
    t.start();
    QAbstractItemModel* m = nullptr;
    while (t.elapsed() < 10000) {
        for (QTableView* v : pane->findChildren<QTableView*>())
            if (qstrcmp(v->metaObject()->className(), "TableView") == 0 && v->model()) m = v->model();
        if (m && m->rowCount() > 0) break;
        pump(50);
    }
    if (!m) { std::printf("no table\n"); return 1; }
    std::printf("rows=%d cols=%d\n", m->rowCount(), m->columnCount());
    // VVPLUGINTEST_SORT=1: click the first header; whether it sorted and
    // what the pane says (a sort over vvg's Memory Limits is refused).
    if (qEnvironmentVariableIsSet("VVPLUGINTEST_SORT")) {
        for (QTableView* v : pane->findChildren<QTableView*>()) {
            if (qstrcmp(v->metaObject()->className(), "TableView") != 0) continue;
            emit v->horizontalHeader()->sectionClicked(0);
            pump(300);
            QString said;
            for (QLabel* l : pane->findChildren<QLabel*>(Qt::FindDirectChildrenOnly))
                if (l->isVisibleTo(pane)) said = l->text();
            std::printf("sorted=%d said=%s\n", v->horizontalHeader()->isSortIndicatorShown() ? 1 : 0,
                        said.isEmpty() ? "-" : said.toUtf8().constData());
            break;
        }
    }
    return m->rowCount() > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc != 4) { std::fprintf(stderr, "usage: vvplugintest --part|--props PLUGIN.so FILE\n"); return 2; }
    const QString mode = QString::fromLocal8Bit(argv[1]);
    const KPluginMetaData md(QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath());
    const QString file = QString::fromLocal8Bit(argv[3]);
    std::printf("mimetypes=%d\n", (int)md.mimeTypes().size());
    if (mode == QLatin1String("--part")) {
        QWidget host;
        auto r = KParts::PartLoader::instantiatePart<KParts::ReadOnlyPart>(md, &host, &host);
        if (!r) { std::printf("part: %s\n", r.errorString.toUtf8().constData()); return 1; }
        if (!r.plugin->openUrl(QUrl::fromLocalFile(file))) { std::printf("openUrl failed\n"); return 1; }
        r.plugin->widget()->show();
        pump(100);
        return report(findPane(r.plugin->widget()));
    }
    if (mode == QLatin1String("--props")) {
        // Only the plugin under test: an installed vvproperties (or any
        // other Properties plugin) would add its own pages.
        QCoreApplication::setLibraryPaths({});
        KPropertiesDialog dlg(QUrl::fromLocalFile(file));
        auto r = KPluginFactory::instantiatePlugin<KPropertiesDialogPlugin>(md, &dlg);
        if (!r) { std::printf("props: %s\n", r.errorString.toUtf8().constData()); return 1; }
        QWidget* pane = findPane(&dlg);
        if (!pane) { std::printf("no Table page\n"); return 1; }
        // Read only when shown: no table while another page is current.
        dlg.show();
        pump(200);
        std::printf("before=%d\n", (int)pane->findChildren<QTableView*>().size());
        if (auto* pm = dlg.findChild<KPageWidgetModel*>()) {
            std::printf("pages=");
            for (int r = 0; r < pm->rowCount(); ++r)
                if (KPageWidgetItem* item = pm->item(pm->index(r, 0)))
                    std::printf("%s%s", r ? "," : "", item->name().toUtf8().constData());
            std::printf("\n");
        }
        if (reportFormat(dlg) != 0) return 1;
        // VVPLUGINTEST_SHOT_FORMAT=out.png: the dialog on its Format page,
        // at a size that shows a column's rows.
        if (const QString shot = qEnvironmentVariable("VVPLUGINTEST_SHOT_FORMAT"); !shot.isEmpty()) {
            dlg.resize(620, 720);
            pump(300);
            dlg.grab().save(shot);
        }
        selectPage(dlg, pane);
        pump(200);
        std::printf("page=%s\n", dlg.currentPage() ? dlg.currentPage()->name().toUtf8().constData() : "-");
        const int rc = report(pane);
        // VVPLUGINTEST_SHOT=out.png: the dialog as shown.
        if (const QString shot = qEnvironmentVariable("VVPLUGINTEST_SHOT"); !shot.isEmpty()) {
            pump(300);
            dlg.grab().save(shot);
        }
        return rc;
    }
    return 2;
}
