#pragma once
#include "settings/DisplaySettings.h"
#include <QPointer>

class QProcess;
class KdeDisplayBackend final : public DisplayBackend {
    Q_OBJECT
public:
    explicit KdeDisplayBackend(QObject *parent = nullptr);
    ~KdeDisplayBackend() override;
    bool rollbackAvailable() const override;
    QString editRestrictionReason() const override;
    void readCurrent() override;
    void beginPreview(const DisplaySnapshot &baseline, const DisplaySnapshot &requested) override;
    void keepPreview() override;
    void revertPreview() override;
private:
    QString helperPath() const;
    void guardMessage();
    QPointer<QProcess> m_reader;
    QPointer<QProcess> m_guard;
    QByteArray m_guardBuffer;
    QJsonObject m_finalMessage;
    bool m_finished = false;
};
