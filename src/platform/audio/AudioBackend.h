#pragma once

#include "settings/AudioSettings.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>
#include <memory>

// Production executes pactl directly with fixed commands. The alternate executable
// is an explicit test boundary; neither commands nor shell programs come from UI.
class PactlAudioBackend final : public AudioBackend {
    Q_OBJECT
public:
    struct Options {
        int processTimeoutMs = 3000;
        int snapshotTimeoutMs = 18000;
        int maxOutputBytes = 4 * 1024 * 1024;
        int pollIntervalMs = 5000;
    };
    explicit PactlAudioBackend(QObject *parent = nullptr);
    PactlAudioBackend(const QString &executable, const Options &options, QObject *parent = nullptr);
    void readCurrent(quint64 token) override;
    void writeChange(quint64 token, const AudioSnapshot &expected, const AudioRequest &request) override;
    static bool parseSnapshot(const QByteArray &info, const QByteArray &sinks,
                              const QByteArray &sources, const QByteArray &cards,
                              const QByteArray &sinkInputs, const QByteArray &sourceOutputs,
                              AudioSnapshot *snapshot, QString *error);
private:
    struct ProcessResult {
        bool success = false, started = false, ambiguous = false;
        QByteArray output;
        QString error;
    };
    struct SnapshotRead;
    using Completion = std::function<void(bool, const AudioSnapshot &, const QString &)>;
    void run(const QStringList &arguments, int timeoutMs,
             const std::function<void(const ProcessResult &)> &done);
    void readSnapshot(const Completion &done);
    void readStep(const std::shared_ptr<SnapshotRead> &read);
    void applyLock(AudioSnapshot *snapshot);
    QStringList command(const AudioRequest &request) const;
    QString m_executable, m_server;
    Options m_options;
    QTimer m_poll;
    QString m_lockedCookie;
    bool m_busy = false;
};
