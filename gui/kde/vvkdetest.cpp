// Headless validation for the KDE plugin payloads — exercises the KF6-free
// cores (thumbnail render + metadata probe) without needing Dolphin or a
// display. Usage: vvkdetest <file> [thumb.png]
//   exit 0 if a non-null thumbnail renders and metadata probes ok.
#include <QGuiApplication>
#include <QImage>
#include <cstdio>

#include "metaprobe.h"
#include "thumbrender.h"

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc < 2) { std::fprintf(stderr, "usage: vvkdetest <file> [out.png]\n"); return 2; }
    QString path = QString::fromLocal8Bit(argv[1]);

    QImage img = vv_render_thumbnail(path, QSize(320, 240));
    std::printf("thumbnail: %s (%dx%d)\n",
                img.isNull() ? "NULL" : "ok", img.width(), img.height());
    if (argc >= 3 && !img.isNull()) img.save(QString::fromLocal8Bit(argv[2]));

    VvMeta m = vv_probe_meta(path);
    std::printf("meta: ok=%d rows=%lld cols=%d\n  schema=%s\n  footer=%s\n  generator=%s\n",
                m.ok, (long long)m.rows, m.cols,
                m.schema.toStdString().c_str(),
                m.footer.toStdString().c_str(),
                m.generator.toStdString().c_str());

    return (img.isNull() || !m.ok) ? 1 : 0;
}
