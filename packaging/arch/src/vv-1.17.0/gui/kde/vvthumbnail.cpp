// KIO::ThumbnailCreator plugin: table-snapshot thumbnails for vv data files
// in Dolphin's icon view / preview pane. Thin wrapper over vv_render_thumbnail
// (which links libvvcore). Installed to $PLUGINDIR/kf6/thumbcreator/.
#include <KIO/ThumbnailCreator>
#include <KPluginFactory>

#include "thumbrender.h"

class VvThumbnailCreator : public KIO::ThumbnailCreator {
    Q_OBJECT
public:
    VvThumbnailCreator(QObject* parent, const QVariantList& args)
        : KIO::ThumbnailCreator(parent, args) {}

    KIO::ThumbnailResult create(const KIO::ThumbnailRequest& request) override {
        QImage img = vv_render_thumbnail(request.url().toLocalFile(),
                                         request.targetSize());
        return img.isNull() ? KIO::ThumbnailResult::fail()
                            : KIO::ThumbnailResult::pass(img);
    }
};

K_PLUGIN_CLASS_WITH_JSON(VvThumbnailCreator, "vvthumbnail.json")

#include "vvthumbnail.moc"
