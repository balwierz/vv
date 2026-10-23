// Headless check of the KF6 part and Properties plugins (KF6 builds only):
//   vvplugintest --part  PLUGIN.so FILE   load the part as a host does, open
//                                         FILE, print its tabs and the first
//                                         one's rows / columns
//   vvplugintest --props PLUGIN.so FILE   build FILE's Properties dialog with
//                                         the plugin, show its Table page, the same
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
#include <QTabBar>
#include <QTableView>
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
        KPropertiesDialog dlg(QUrl::fromLocalFile(file));
        auto r = KPluginFactory::instantiatePlugin<KPropertiesDialogPlugin>(md, &dlg);
        if (!r) { std::printf("props: %s\n", r.errorString.toUtf8().constData()); return 1; }
        QWidget* pane = findPane(&dlg);
        if (!pane) { std::printf("no Table page\n"); return 1; }
        // Read only when shown: no table while another page is current.
        dlg.show();
        pump(200);
        std::printf("before=%d\n", (int)pane->findChildren<QTableView*>().size());
        if (auto* pm = dlg.findChild<KPageWidgetModel*>())
            for (int r = 0; r < pm->rowCount(); ++r)
                if (KPageWidgetItem* item = pm->item(pm->index(r, 0)); item && item->widget() == pane)
                    dlg.setCurrentPage(item);
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
