#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

enum class AudioObjectKind { Sink, Source, SinkInput, SourceOutput, Card };
enum class AudioAction { SetDefault, SetVolume, SetChannelVolumes, SetMute, SetPort, SetProfile, MoveStream };
enum class AudioOutcome { Success, Failed, Uncertain };

struct AudioChoice {
    QString name, description, availability;
};

struct AudioObject {
    AudioObjectKind kind = AudioObjectKind::Sink;
    quint32 index = 0;
    QString name, description, identity;
    QString activePort, activeProfile;
    QVector<AudioChoice> ports, profiles;
    QVector<quint32> channelVolumes;
    QStringList channelNames;
    bool mute = false, volumeWritable = true, muteWritable = true;
    quint32 cardIndex = 0, routeIndex = 0;
    bool hasCard = false, hasRoute = false;
    QVariantMap properties;
    int volumePercent() const;
};

struct AudioSnapshot {
    QString serverCookie, serverName, serverVersion, defaultSink, defaultSource;
    QVector<AudioObject> sinks, sources, cards, sinkInputs, sourceOutputs;
    bool available = false, writable = false;
    QString unavailableReason;
    const AudioObject *object(AudioObjectKind kind, quint32 index) const;
    const QVector<AudioObject> &objects(AudioObjectKind kind) const;
};

struct AudioRequest {
    AudioAction action = AudioAction::SetVolume;
    AudioObjectKind kind = AudioObjectKind::Sink;
    quint32 index = 0;
    QString identity, name;
    int volumePercent = 100;
    bool mute = false;
    QString choice;
    quint32 destinationIndex = 0;
    QString destinationIdentity;
    QVector<quint32> channelVolumes;
};

namespace AudioTypes {
constexpr quint32 NormalVolume = 65536;
constexpr quint32 MaximumVolume = 98304;
QVector<quint32> scaledVolumes(const AudioObject &object, int percent);
bool validateRequest(const AudioSnapshot &snapshot, const AudioRequest &request, QString &error);
bool sameTarget(const AudioSnapshot &expected, const AudioSnapshot &current, const AudioRequest &request);
bool matchesRequest(const AudioSnapshot &snapshot, const AudioRequest &request);
}

class AudioBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent(quint64 token) = 0;
    virtual void writeChange(quint64 token, const AudioSnapshot &expected, const AudioRequest &request) = 0;
signals:
    void readFinished(quint64 token, bool success, const AudioSnapshot &snapshot, const QString &error);
    void writeFinished(quint64 token, AudioOutcome outcome, const AudioSnapshot &snapshot, const QString &error);
    void externalChange();
};

// Immediate audio operations belong to the application, not a Sound dialog.
// The UI always reflects confirmed server values and retained outcomes.
class AudioSettings final : public QObject {
    Q_OBJECT
public:
    static AudioSettings *instance();
    explicit AudioSettings(AudioBackend *backend, QObject *parent = nullptr);
    const AudioSnapshot &snapshot() const { return m_snapshot; }
    bool isReady() const { return m_snapshot.available; }
    bool isBusy() const { return m_busy; }
    QString errorText() const { return m_error; }
    QString resultText() const { return m_result; }
    QString supportText() const { return m_snapshot.unavailableReason; }
public slots:
    void refresh();
    void requestChange(const AudioRequest &request);
signals:
    void stateChanged();
private:
    void busy(bool active);
    AudioBackend *m_backend;
    AudioSnapshot m_snapshot;
    AudioRequest m_request;
    QString m_error, m_result, m_lockedCookie, m_lockReason;
    quint64 m_token = 0;
    bool m_busy = false, m_writing = false, m_refreshPending = false, m_readFailed = false;
};

Q_DECLARE_METATYPE(AudioObjectKind)
Q_DECLARE_METATYPE(AudioAction)
Q_DECLARE_METATYPE(AudioOutcome)
Q_DECLARE_METATYPE(AudioSnapshot)
Q_DECLARE_METATYPE(AudioRequest)
