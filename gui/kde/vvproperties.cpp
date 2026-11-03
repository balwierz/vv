// KPropertiesDialogPlugin: two pages in the Properties dialog of a data file
// (Dolphin, the file dialogs), each read only when it is first shown:
//   Format  how the file is stored — file_details(), as `vv --info` prints
//           it (DetailsTree), read on a worker thread. Not "Details": Plasma's
//           baloo-widgets adds a page of that name.
//   Table   the file as tables (VvTablePane).
// Installed to $PLUGINDIR/kf6/propertiesdialog/.
#include <KFileItem>
#include <KPluginFactory>
#include <KPropertiesDialog>
#include <KPropertiesDialogPlugin>

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

#include "detailstree.h"
#include "tablepane.h"

namespace {

// The file's details, or its open error as the one row of an "Error"
// section. The source is opened and dropped on this (worker) thread.
FileDetails read_details(const std::string& path) {
    try {
        Config cfg;
        cfg.path = path;
        std::unique_ptr<TabularSource> src;
        const std::string e = open_source(path, cfg, &src);
        if (!e.empty() || !src)
            return {{"Error", "", {{"vv cannot read this file", e.empty() ? "nothing to show" : e, {}}}}};
        return file_details(*src);
    } catch (const std::exception& ex) {
        return {{"Error", "", {{"vv cannot read this file", ex.what(), {}}}}};
    }
}

}  // namespace

class VvPropertiesPlugin : public KPropertiesDialogPlugin {
    Q_OBJECT
public:
    VvPropertiesPlugin(QObject* parent, const QVariantList&) : KPropertiesDialogPlugin(parent) {
        const KFileItemList items = properties->items();
        if (items.size() != 1) return;                 // one file's pages only
        const QString path = items.first().localPath();
        if (path.isEmpty() || items.first().isDir()) return;

        auto* tree = new DetailsTree(/*ownCopyShortcut=*/true);
        KPageWidgetItem* format = properties->addPage(tree, tr("Format"));
        auto* watcher = new QFutureWatcher<FileDetails>(tree);
        connect(watcher, &QFutureWatcherBase::finished, tree,
                [tree, watcher] { tree->setDetails(watcher->result()); });

        auto* pane = new VvTablePane(/*filterBox=*/false);
        KPageWidgetItem* table = properties->addPage(pane, tr("Table"));

        // Read when a page is first selected: Properties opened for the
        // other pages reads nothing. A result arriving after the dialog
        // closed goes with the watcher.
        connect(properties, &KPageDialog::currentPageChanged, this,
                [=](KPageWidgetItem* current) {
                    if (current == format && !tree->hasDetails() && !watcher->isRunning()) {
                        tree->showMessage(tr("Reading…"));
                        watcher->setFuture(QtConcurrent::run(read_details, path.toStdString()));
                    }
                    if (current == table && pane->tabCount() == 0) pane->open(path);
                });
    }
};

K_PLUGIN_CLASS_WITH_JSON(VvPropertiesPlugin, "vvproperties.json")

#include "vvproperties.moc"
