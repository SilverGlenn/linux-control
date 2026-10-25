#pragma once

#include "SettingsSession.h"
#include "NetworkTypes.h"
#include <QElapsedTimer>
#include <QTimer>

class NetworkBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent() = 0;
    virtual void scan(const QString &devicePath) = 0;
    virtual void beginChange(const NetworkSnapshot &baseline, const NetworkRequest &request) = 0;
    virtual void keepChange() = 0;
    virtual void revertChange() = 0;
    virtual QString connectivityRestrictionReason() const { return {}; }
signals:
    void readFinished(bool success, const NetworkSnapshot &snapshot, const QString &error);
    void scanFinished(bool success, const QString &error);
    void previewReady(const NetworkSnapshot &snapshot, int seconds);
    void transactionFinished(bool kept, bool restored, const NetworkSnapshot &snapshot, const QString &error);
    void externalChange();
};

class NetworkSettings : public SettingsSession {
    Q_OBJECT
public:
    static NetworkSettings *instance();
    explicit NetworkSettings(NetworkBackend *backend, QObject *parent = nullptr);
    const NetworkSnapshot &snapshot() const { return m_snapshot; }
    bool isReady() const { return m_ready; }
    bool rollbackAvailable() const { return m_snapshot.checkpointAvailable; }
    bool awaitingConfirmation() const { return m_confirming; }
    bool isScanning() const { return m_scanning; }
    int remainingSeconds() const;
    QString selectedProfile() const { return m_profile; }
    const NetworkPreferences &stagedPreferences() const { return m_staged; }
    QString supportText() const;
    QString connectivityRestrictionReason() const { return m_backend->connectivityRestrictionReason(); }
    QString selectedProfileEditReason() const;
public slots:
    void selectProfile(const QString &path);
    void stagePreferences(const NetworkPreferences &preferences);
    void apply() override;
    void revert() override;
    void keep();
    void refresh();
    void scan(const QString &devicePath);
    void activateProfile(const QString &profilePath, const QString &devicePath);
    void connectWifi(const QString &accessPointPath, const QString &password);
    void disconnectDevice(const QString &devicePath);
private:
    enum class Phase { Reading, Idle, Preflight, Preview };
    void requestChange(NetworkRequest request);
    void readDone(bool success, const NetworkSnapshot &snapshot, const QString &error);
    void finish(bool kept, bool restored, const NetworkSnapshot &snapshot, const QString &error);
    void adopt(const NetworkSnapshot &snapshot, bool preserveStaged);
    NetworkBackend *m_backend;
    NetworkSnapshot m_snapshot;
    NetworkSnapshot m_baseline;
    NetworkRequest m_request;
    NetworkPreferences m_staged;
    QString m_profile;
    Phase m_phase = Phase::Reading;
    bool m_ready = false;
    bool m_confirming = false;
    bool m_resolutionPending = false;
    bool m_scanning = false;
    bool m_refreshPending = false;
    bool m_discardRequested = false;
    int m_seconds = 0;
    QTimer m_ticks;
    QElapsedTimer m_elapsed;
};
