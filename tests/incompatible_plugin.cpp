#include <QObject>

class IncompatibleExtension : public QObject {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.twinfile.Extension/1.0" FILE "incompatible_plugin.json")
};

#include "incompatible_plugin.moc"
