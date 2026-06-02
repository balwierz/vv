// Probe a vv-supported data file for displayable metadata. KF6-free
// (libvvcore only) so it's unit-testable and reusable by the
// KFileMetaData::ExtractorPlugin. Cheap: never drains streaming sources —
// rowCount is -1 when the format can't report it without a full scan.
#pragma once
#include <QString>
#include <cstdint>

struct VvMeta {
    bool        ok = false;
    int64_t     rows = -1;        // -1 = not known without a full scan
    int         cols = 0;
    QString     schema;           // "name:type, name:type, …" (capped)
    QString     footer;           // source footer (codec, row groups, …)
    QString     generator;        // created_by(), if any
};

VvMeta vv_probe_meta(const QString& path);
