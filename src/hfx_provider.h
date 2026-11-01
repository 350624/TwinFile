#pragma once

#include "hfx.h"

namespace twinfile {

// Opaque, process-local endpoints keep WLAN-only sessions separate from ADB serials.
bool isHfxEndpoint(const QString &endpoint);
QString registerHfxSession(const std::shared_ptr<HfxSession> &session);
void unregisterHfxSession(const QString &endpoint);
std::shared_ptr<HfxSession> findHfxSession(const QString &endpoint);
Listing listHfx(const QString &endpoint, const QString &path, bool hidden, const Cancellation &cancel = {});
bool usesHfx(const QList<Entry> &entries, const Location &destination);
Result transferHfx(const QList<Entry> &entries, const Location &destination, bool move, const Cancellation &cancel = {}, const Progress &progress = {});

}
