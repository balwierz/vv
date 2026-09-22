// Work budget for the KDE plugin payloads (thumbnailer + metadata extractor).
// They run in Dolphin's preview worker and in Baloo on every file the user
// browses or indexes, so each call must finish in about a second and a few
// hundred MB.
//
// Most formats meet that whatever the file size, because the payload reads a
// bounded prefix: one CSV / JSON block, one Parquet row group, the BAM / VCF
// header plus the first records, an HDF5 / AnnData capped preview. A few
// readers instead parse or inflate a whole part of the file at open, so their
// cost grows with the file:
//
//   .xlsx / .xlsm   the whole first sheet      ~64 ms and ~17 MB RSS per MB
//   .ods            the whole content.xml      ~95 ms and ~65 MB RSS per MB
//   .npz            the whole first array      ~3 ms and ~2 MB RSS per MB
//
// (measured on an 8-core desktop). vv_within_plugin_budget() refuses those
// above a per-format size ceiling that keeps them near one second, and the
// plugins then produce nothing (Dolphin shows the MIME-type icon). There is no
// wall-clock timeout: in-process decoding cannot be cancelled safely, so the
// budget is enforced by never starting work known to be unbounded.
#pragma once
#include <QString>

bool vv_within_plugin_budget(const QString& path);
