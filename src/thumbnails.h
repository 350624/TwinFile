#pragma once

#include "core.h"
#include <QImage>
#include <QThreadPool>

namespace twinfile {

bool supportsThumbnail(const Entry &entry);
QList<int> thumbnailLoadOrder(int first, int visibleCount, int total, bool backwards);
QString thumbnailCacheKey(const Entry &entry);
QImage cachedThumbnail(const Entry &entry);
QImage loadThumbnail(const Entry &entry, const Cancellation &cancel = {});
QThreadPool *thumbnailPool();

}
