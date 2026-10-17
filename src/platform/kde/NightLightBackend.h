#pragma once

#include "settings/NightLightSettings.h"
#include <QFileSystemWatcher>
#include <QTimer>
#include <QPointer>
#include <functional>

class QDBusConnection;

// The transport owns no configuration files. Tests inject this service and an
// isolated KConfig context; production uses the owner-pinned native D-Bus API.
class NightLightService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void query(quint64 token, const QString &expectedOwner = {}) = 0;
    virtual void preview(quint64 token, int temperature, const QString &expectedOwner, quint64 expectedEpoch) = 0;
    virtual void stopPreview(quint64 token, const QString &expectedOwner, quint64 expectedEpoch) = 0;
    // Pure local retirement, never global Stop. False proves this transport
    // has no preview RPC sent for its current provider; default is conservative.
    virtual bool retirePendingPreview() { return true; }
signals:
    void queryFinished(quint64 token, bool success, const NightLightRuntime &runtime, const QString &error);
    void previewFinished(quint64 token, bool success, bool stopped, const QString &error);
    void externalChange();
    // Name-owner/registration loss; ordinary properties changes are not this signal.
    void providerChanged();
};
bool validateNightLightSchema(const QByteArray &xml, QString &error);
NightLightService *createNightLightService(const QDBusConnection &connection, QObject *parent = nullptr);
// Private-bus fixtures provide exact schema evidence without inspecting the host.
NightLightService *createNightLightService(const QDBusConnection &connection, const QByteArray &schema, QObject *parent = nullptr);

class NightLightWriteJob;

class KdeNightLightBackend final : public NightLightBackend {
    Q_OBJECT
public:
    explicit KdeNightLightBackend(QObject *parent = nullptr);
    struct ConfigContext {
        bool fullCascade = false;
        QStringList defaultSources;
        bool fixture = false;
        std::function<void()> beforeFileRead,beforeSync,afterSync;
        int softDeadlineMs = 20000;
        int previewTrackingMs = 15000;
    };
    KdeNightLightBackend(const QString &absoluteFile, NightLightService *service,
                        const ConfigContext &context, QObject *parent = nullptr);
    ~KdeNightLightBackend() override;
    void readCurrent(quint64 token) override;
    void writeChanges(quint64 token, const NightLightSnapshot &expected,
                      const NightLightSnapshot &requested) override;
    void preview(quint64 token, int temperature, const QString &expectedOwner, quint64 expectedEpoch) override;
    void stopPreview(quint64 token, const QString &expectedOwner, quint64 expectedEpoch) override;
private:
    enum class Phase { Idle, ReadQuery, ReadFiles, Write };
    void queryFinished(quint64 token, bool success, const NightLightRuntime &runtime, const QString &error);
    void retireLocalPreview(const QString &reason, bool clearLease, bool notifyAwaiting);
    void watchFiles();
    QStringList relevantFiles() const;
    QString m_file;
    ConfigContext m_context;
    NightLightService *m_service;
    Phase m_phase = Phase::Idle;
    quint64 m_token = 0;
    QPointer<NightLightWriteJob> m_writeJob;
    QString m_previewOwner;
    quint64 m_previewToken = 0, m_previewEpoch = 0, m_previewGeneration = 0;
    bool m_previewMayBeActive = false;
    bool m_previewAwaitingReply = false, m_previewAwaitingStop = false, m_previewReplyRetired = false;
    bool m_previewLeaseUncertain = false;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce, m_deadline, m_previewExpiry;
};
