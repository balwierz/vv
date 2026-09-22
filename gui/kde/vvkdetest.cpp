// Headless validation for the KDE plugin payloads — exercises the KF6-free
// cores (thumbnail render + metadata probe) without needing Dolphin or a
// display. Usage: vvkdetest [--allow-empty] <file> [thumb.png]
//   Default:        exit 0 iff a non-null thumbnail renders AND metadata probes ok.
//   --allow-empty:  exit 0 as long as both cores RETURN (a null thumbnail / not-ok
//                   meta is fine) — i.e. a crash-safety check that a malformed file
//                   is handled gracefully instead of aborting the worker.
//   --mime FILE...:  print "<file>\t<MIME type>" as QMimeDatabase resolves each
//                    file (name + content), the lookup Dolphin / KIO use to pick
//                    the thumbnailer, extractor and "Open with" applications.
//   --mime-types NAME...: exit 1 unless every NAME is a known MIME type.
#include <QGuiApplication>
#include <QImage>
#include <QElapsedTimer>
#include <QMimeDatabase>
#include <cstdio>

#include "metaprobe.h"
#include "thumbrender.h"

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc >= 2 && (QByteArray(argv[1]) == "--mime" || QByteArray(argv[1]) == "--mime-types")) {
        const bool byName = QByteArray(argv[1]) == "--mime-types";
        QMimeDatabase db;
        int bad = 0;
        for (int i = 2; i < argc; ++i) {
            const QString a = QString::fromLocal8Bit(argv[i]);
            if (byName) {
                if (!db.mimeTypeForName(a).isValid()) {
                    std::printf("unknown MIME type: %s\n", argv[i]);
                    ++bad;
                }
            } else {
                std::printf("%s\t%s\n", argv[i],
                            db.mimeTypeForFile(a).name().toUtf8().constData());
            }
        }
        return bad ? 1 : 0;
    }
    bool allowEmpty = false;
    QString path, out;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--allow-empty") allowEmpty = true;
        else if (path.isEmpty())  path = a;
        else                      out = a;
    }
    if (path.isEmpty()) {
        std::fprintf(stderr, "usage: vvkdetest [--allow-empty] <file> [out.png]\n");
        return 2;
    }

    QElapsedTimer clock;
    clock.start();
    QImage img = vv_render_thumbnail(path, QSize(320, 240));
    std::printf("thumbnail: %s (%dx%d) %lld ms\n",
                img.isNull() ? "NULL" : "ok", img.width(), img.height(),
                (long long)clock.restart());
    if (!out.isEmpty() && !img.isNull()) img.save(out);

    VvMeta m = vv_probe_meta(path);
    std::printf("meta: ok=%d rows=%lld cols=%d %lld ms\n  schema=%s\n  footer=%s\n  generator=%s\n  genomic=%s\n",
                m.ok, (long long)m.rows, m.cols, (long long)clock.elapsed(),
                m.schema.toStdString().c_str(),
                m.footer.toStdString().c_str(),
                m.generator.toStdString().c_str(),
                m.genomic.toStdString().c_str());

    // --allow-empty: reaching here without aborting is the success condition.
    if (allowEmpty) return 0;
    return (img.isNull() || !m.ok) ? 1 : 0;
}
