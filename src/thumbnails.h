#pragma once

#include "core.h"
#include <QImage>
#include <QThreadPool>

namespace twinfile {

bool supportsThumbnail(const Entry &entry);
QString thumbnailCacheKey(const Entry &entry);
QImage cachedThumbnail(const Entry &entry);
QImage loadThumbnail(const Entry &entry, const Cancellation &cancel = {});
QThreadPool *thumbnailPool();

}
