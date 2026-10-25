#pragma once

#include "core.h"
#include <QDialog>

namespace twinfile {

struct EntryStatistics {
    qint64 bytes = 0;
    qint64 files = 0;
    qint64 directories = 0;
    qint64 links = 0;
    QString error;
};

EntryStatistics inspectEntries(const QList<Entry> &entries, const Cancellation &cancel = {});

class PropertiesDialog : public QDialog {
public:
    explicit PropertiesDialog(const QList<Entry> &entries, QWidget *parent = nullptr);
    ~PropertiesDialog() override;
protected:
    void done(int result) override;
private:
    Cancellation cancel_ = std::make_shared<std::atomic_bool>(false);
};

}
