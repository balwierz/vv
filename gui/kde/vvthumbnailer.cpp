// vv-thumbnailer: table-snapshot thumbnails for the file managers that run
// freedesktop thumbnailers (vv.thumbnailer): Nautilus, Nemo, Caja, and
// Thunar through tumbler. The same image Dolphin's plugin shows
// (vv_render_thumbnail, with its per-format size budget), without KF6.
//
//   vv-thumbnailer -s SIZE INPUT OUTPUT.png
//
// SIZE bounds the longer side. GNOME runs thumbnailers inside bubblewrap
// with no display, so Qt is put on its offscreen platform. Exit 0 and a PNG
// when a thumbnail renders; 1 and no file when the input is unreadable or
// over its format's budget; 2 on a usage error.
#include <QGuiApplication>
#include <QImage>
#include <QSaveFile>
#include <QStringList>

#include <cstdio>

#include "thumbrender.h"

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    int size = 256;
    QStringList files;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("-s") && i + 1 < args.size()) {
            bool ok = false;
            size = args[++i].toInt(&ok);
            if (!ok || size < 16 || size > 4096) size = 0;
        } else {
            files << args[i];
        }
    }
    if (files.size() != 2 || size == 0) {
        std::fprintf(stderr, "usage: vv-thumbnailer -s SIZE INPUT OUTPUT.png\n");
        return 2;
    }
    const QImage img = vv_render_thumbnail(files[0], QSize(size, size));
    if (img.isNull()) return 1;
    // Written whole or not at all: a file manager caches what it finds.
    QSaveFile out(files[1]);
    if (!out.open(QIODevice::WriteOnly) || !img.save(&out, "PNG") || !out.commit()) {
        std::fprintf(stderr, "vv-thumbnailer: cannot write %s\n", argv[argc - 1]);
        return 1;
    }
    return 0;
}
