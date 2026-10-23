// KPropertiesDialogPlugin: a "Table" page in the Properties dialog of a data
// file (Dolphin, the file dialogs): the file as tables (VvTablePane), read
// only when the page is first shown. Installed to
// $PLUGINDIR/kf6/propertiesdialog/.
#include <KFileItem>
#include <KPluginFactory>
#include <KPropertiesDialog>
#include <KPropertiesDialogPlugin>

#include "tablepane.h"

class VvPropertiesPlugin : public KPropertiesDialogPlugin {
    Q_OBJECT
public:
    VvPropertiesPlugin(QObject* parent, const QVariantList&) : KPropertiesDialogPlugin(parent) {
        const KFileItemList items = properties->items();
        if (items.size() != 1) return;                 // one file's page only
        const QString path = items.first().localPath();
        if (path.isEmpty() || items.first().isDir()) return;
        auto* pane = new VvTablePane(/*filterBox=*/false);
        KPageWidgetItem* page = properties->addPage(pane, tr("Table"));
        // Read when the page is first selected: Properties opened for the
        // other pages reads nothing.
        connect(properties, &KPageDialog::currentPageChanged, pane,
                [pane, page, path](KPageWidgetItem* current) {
                    if (current == page && pane->tabCount() == 0) pane->open(path);
                });
    }
};

K_PLUGIN_CLASS_WITH_JSON(VvPropertiesPlugin, "vvproperties.json")

#include "vvproperties.moc"
