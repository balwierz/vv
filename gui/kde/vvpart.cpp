// KParts::ReadOnlyPart: a data file as tables (VvTablePane, with a filter
// box) inside a KParts host — Konqueror for a file it embeds, Ark for a file
// in an archive. The host hands the part a local file (a remote URL arrives
// as a temporary copy). Installed to $PLUGINDIR/kf6/parts/.
#include <KParts/ReadOnlyPart>
#include <KPluginFactory>

#include "tablepane.h"

class VvPart : public KParts::ReadOnlyPart {
    Q_OBJECT
public:
    VvPart(QWidget* parentWidget, QObject* parent, const KPluginMetaData& md, const QVariantList&)
        : KParts::ReadOnlyPart(parent, md), pane_(new VvTablePane(/*filterBox=*/true, parentWidget)) {
        setWidget(pane_);
    }

protected:
    // A file vv cannot read still opens: the pane says why.
    bool openFile() override {
        pane_->open(localFilePath());
        return true;
    }
    bool closeUrl() override {
        pane_->clear();
        return KParts::ReadOnlyPart::closeUrl();
    }

private:
    VvTablePane* pane_;
};

K_PLUGIN_CLASS_WITH_JSON(VvPart, "vvpart.json")

#include "vvpart.moc"
