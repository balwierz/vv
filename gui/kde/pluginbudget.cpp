#include "pluginbudget.h"

#include <QFileInfo>
#include <QStringList>

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
    else if (ext == QLatin1String("rds") || ext == QLatin1String("rdata") ||
             ext == QLatin1String("rda"))
        ceiling = 32 * MiB;
    else if (ext == QLatin1String("gb") || ext == QLatin1String("gbk") ||
             ext == QLatin1String("gbff") || ext == QLatin1String("genbank") ||
             ext == QLatin1String("embl"))
        ceiling = 96 * MiB;
    else if (QStringList{QStringLiteral("sto"), QStringLiteral("stk"), QStringLiteral("stockholm"),
                         QStringLiteral("aln"), QStringLiteral("clustal"), QStringLiteral("clw"),
                         QStringLiteral("clustalw"), QStringLiteral("phy"), QStringLiteral("phylip"),
                         QStringLiteral("nex"), QStringLiteral("nxs"), QStringLiteral("nexus"),
                         QStringLiteral("msf"), QStringLiteral("a2m"), QStringLiteral("a3m"),
                         QStringLiteral("afa"), QStringLiteral("afasta")}.contains(ext))
        ceiling = 64 * MiB;   // alignments are read whole; a Pfam "full" one is GBs
    return ceiling < 0 || fi.size() <= ceiling;
}
