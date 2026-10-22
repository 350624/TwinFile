#pragma once

#include "core.h"
#include <QHostAddress>
#include <mutex>

namespace twinfile {

struct HfxOptions {
    QString serial;
    QList<QPair<QString, QHostAddress>> interfaces;
    quint16 port = 5740;
};

class HfxSession {
public:
    HfxSession();
    ~HfxSession();
    Result connect(const HfxOptions &options, const Cancellation &cancel = {}, std::function<void(quint16)> listening = {});
    Result push(const QStringList &paths, const QString &destination, const Cancellation &cancel = {}, const Progress &progress = {});
    Result pull(const QStringList &paths, const QString &source, const QString &destination, const Cancellation &cancel = {}, const Progress &progress = {});
    Listing list(const QString &path, const Cancellation &cancel = {});
    Result mkdir(const QString &parent, const QString &name, const Cancellation &cancel = {});
    Result remove(const QString &path, const Cancellation &cancel = {});
    void close();
    bool connected() const;
    QString home() const;
private:
    struct State;
    std::unique_ptr<State> state_;
    std::mutex operation_;
};

}
