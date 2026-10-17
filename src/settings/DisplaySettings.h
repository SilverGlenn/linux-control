#pragma once

#include "SettingsSession.h"
#include "display/DisplayProtocol.h"
#include <QTimer>
#include <QElapsedTimer>
#include <functional>

class DisplayBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool rollbackAvailable() const = 0;
    virtual void readCurrent() = 0;
    virtual void beginPreview(const DisplaySnapshot &baseline, const DisplaySnapshot &requested) = 0;
    virtual void keepPreview() = 0;
    virtual void revertPreview() = 0;
signals:
    void readFinished(bool success, const DisplaySnapshot &snapshot, const QString &error);
    // Emitted only after helper readback; helper independently owns the deadline.
    void previewReady(const DisplaySnapshot &snapshot, int seconds);
    // Final snapshot is authoritative; invalid means state could not be read.
    void transactionFinished(bool kept, bool restored, const DisplaySnapshot &snapshot,
                             const QString &error);
    void externalChange();
};

class DisplaySettings : public SettingsSession {
    Q_OBJECT
public:
    static DisplaySettings *instance();
    explicit DisplaySettings(DisplayBackend *backend, QObject *parent = nullptr);
    const DisplaySnapshot &snapshot() const { return m_snapshot; }
    const DisplaySnapshot &staged() const { return m_staged; }
    bool isReady() const { return m_ready; }
    bool rollbackAvailable() const { return m_backend->rollbackAvailable(); }
    bool awaitingConfirmation() const { return m_confirming; }
    int remainingSeconds() const;
    int selectedOutput() const { return m_output; }
    QString stagedMode() const;
    QString supportText() const;
    QString selectedSupportText() const;
public slots:
    void selectOutput(int id);
    void selectMode(const QString &mode);
    void selectEnabled(bool enabled);
    void selectScale(double scale);
    void selectRotation(int rotation);
    void selectPosition(int x, int y);
    void apply() override;
    void revert() override;
    void keep();
    void refresh();
private:
    enum class Phase { Reading, Idle, Preflight, Preview };
    void readDone(bool success, const DisplaySnapshot &snapshot, const QString &error);
    void finish(bool kept, bool restored, const DisplaySnapshot &snapshot, const QString &error);
    void adopt(const DisplaySnapshot &snapshot, bool keepSelection);
    void editSelected(const std::function<void(DisplayOutput &)> &edit);
    void finishPendingRefresh();
    DisplayBackend *m_backend;
    DisplaySnapshot m_snapshot, m_staged;
    Phase m_phase = Phase::Reading;
    int m_output = 0;
    bool m_ready = false;
    bool m_confirming = false;
    bool m_resolutionPending = false;
    bool m_refreshPending = false;
    bool m_reverting = false;
    int m_seconds = 0;
    QTimer m_ticks;
    QElapsedTimer m_elapsed;
};

Q_DECLARE_METATYPE(DisplaySnapshot)
