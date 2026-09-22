#include "pluginbudget.h"

#include <QFileInfo>

bool vv_within_plugin_budget(const QString& path) {
    constexpr qint64 MiB = 1024 * 1024;
    const QFileInfo fi(path);
    const QString ext = fi.suffix().toLower();
    qint64 ceiling = -1;                                   // -1 = no ceiling
    if (ext == QLatin1String("xlsx") || ext == QLatin1String("xlsm"))
        ceiling = 16 * MiB;
    else if (ext == QLatin1String("ods"))
        ceiling = 8 * MiB;
    else if (ext == QLatin1String("npz"))
        ceiling = 256 * MiB;
    return ceiling < 0 || fi.size() <= ceiling;
}
