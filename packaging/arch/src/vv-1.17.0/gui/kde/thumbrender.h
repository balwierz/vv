// Render a small table-snapshot thumbnail for a vv-supported data file.
// KF6-free (Qt Gui + libvvcore only) so it can be unit-tested directly and
// reused by the KIO::ThumbnailCreator plugin. Returns a null QImage on
// failure (unreadable / unsupported file).
#pragma once
#include <QImage>
#include <QSize>
#include <QString>

QImage vv_render_thumbnail(const QString& path, const QSize& target);
