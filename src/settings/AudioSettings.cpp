#include "AudioSettings.h"
#include "platform/audio/AudioBackend.h"
#include "operations/OperationManager.h"

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <algorithm>

namespace {
bool endpoint(AudioObjectKind kind) { return kind == AudioObjectKind::Sink || kind == AudioObjectKind::Source; }
bool stream(AudioObjectKind kind) { return kind == AudioObjectKind::SinkInput || kind == AudioObjectKind::SourceOutput; }
bool safeArgument(const QString &value)
{ return !value.isEmpty() && value.size() <= 1024 && !value.contains(QChar::Null) && !value.startsWith('-'); }
bool usableChoice(const QVector<AudioChoice> &choices, const QString &name)
{
    for (const auto &choice : choices)
        if (choice.name == name && safeArgument(name)) {
            const auto availability = choice.availability.toLower();
            return availability != QLatin1String("no") && availability != QLatin1String("not available")
                && availability != QLatin1String("unavailable");
        }
    return false;
}
bool sameIdentity(const AudioObject *before, const AudioObject *after)
{
    return before && after && !before->identity.isEmpty() && before->identity == after->identity
        && before->name == after->name && before->index == after->index && before->kind == after->kind;
}
bool sameVolumes(const QVector<quint32> &expected, const QVector<quint32> &actual)
{
    if (expected.isEmpty() || expected.size() != actual.size()) return false;
    // The server can round a channel by one PulseAudio volume unit.
    for (qsizetype i = 0; i < expected.size(); ++i)
        if (qAbs(qint64(expected[i]) - qint64(actual[i])) > 1) return false;
    return true;
}
}

int AudioObject::volumePercent() const
{
    if (channelVolumes.isEmpty()) return 0;
    return int((quint64(*std::max_element(channelVolumes.begin(), channelVolumes.end())) * 100 + AudioTypes::NormalVolume / 2)
               / AudioTypes::NormalVolume);
}

const QVector<AudioObject> &AudioSnapshot::objects(AudioObjectKind kind) const
{
    switch (kind) {
    case AudioObjectKind::Sink: return sinks;
    case AudioObjectKind::Source: return sources;
    case AudioObjectKind::SinkInput: return sinkInputs;
    case AudioObjectKind::SourceOutput: return sourceOutputs;
    case AudioObjectKind::Card: return cards;
    }
    static const QVector<AudioObject> empty;
    return empty;
}

const AudioObject *AudioSnapshot::object(AudioObjectKind kind, quint32 index) const
{
    for (const auto &entry : objects(kind)) if (entry.index == index) return &entry;
    return nullptr;
}

QVector<quint32> AudioTypes::scaledVolumes(const AudioObject &object, int percent)
{
    if (percent < 0 || percent > 150 || object.channelVolumes.isEmpty()) return {};
    const quint64 target = (quint64(percent) * NormalVolume + 50) / 100;
    const quint64 peak = *std::max_element(object.channelVolumes.begin(), object.channelVolumes.end());
    QVector<quint32> result;
    for (const auto volume : object.channelVolumes)
        result << quint32(peak ? (quint64(volume) * target + peak / 2) / peak : target);
    return result;
}

bool AudioTypes::validateRequest(const AudioSnapshot &snapshot, const AudioRequest &request, QString &error)
{
    error.clear();
    auto fail = [&](const QString &message) { error = message; return false; };
    if (!snapshot.available || !snapshot.writable || snapshot.serverCookie.isEmpty())
        return fail(snapshot.unavailableReason.isEmpty() ? QStringLiteral("The audio server is unavailable or cannot safely accept changes.") : snapshot.unavailableReason);
    const auto *object = snapshot.object(request.kind, request.index);
    if (!object || object->identity != request.identity)
        return fail(QStringLiteral("The selected audio device or application is no longer available. Refresh the list."));
    if (object->identity.isEmpty())
        return fail(QStringLiteral("This audio object's lifetime identity could not be verified. Its controls are read-only."));
    switch (request.action) {
    case AudioAction::SetDefault:
        if (!endpoint(request.kind) || !safeArgument(object->name)) return fail(QStringLiteral("Select a playback or recording device."));
        return true;
    case AudioAction::SetVolume:
    case AudioAction::SetChannelVolumes:
        if ((!endpoint(request.kind) && !stream(request.kind)) || !object->volumeWritable
            || object->channelVolumes.isEmpty() || object->channelVolumes.size() > 32)
            return fail(QStringLiteral("Volume is unavailable for this audio object."));
        if (request.action == AudioAction::SetVolume)
            return request.volumePercent >= 0 && request.volumePercent <= 150 ? true : fail(QStringLiteral("Use a volume between 0% and 150%."));
        if (request.channelVolumes.size() != object->channelVolumes.size())
            return fail(QStringLiteral("The audio channel layout changed. Refresh before setting levels."));
        for (const auto volume : request.channelVolumes)
            if (volume > MaximumVolume) return fail(QStringLiteral("Use channel levels between 0% and 150%."));
        return true;
    case AudioAction::SetMute:
        if ((!endpoint(request.kind) && !stream(request.kind)) || !object->muteWritable)
            return fail(QStringLiteral("Mute is unavailable for this audio object."));
        return true;
    case AudioAction::SetPort:
        return endpoint(request.kind) && usableChoice(object->ports, request.choice) ? true
            : fail(QStringLiteral("The selected audio port is unavailable."));
    case AudioAction::SetProfile:
        return request.kind == AudioObjectKind::Card && usableChoice(object->profiles, request.choice) ? true
            : fail(QStringLiteral("The selected sound-card profile is unavailable."));
    case AudioAction::MoveStream: {
        if (!stream(request.kind)) return fail(QStringLiteral("Select a playback or recording application."));
        const auto destinationKind = request.kind == AudioObjectKind::SinkInput ? AudioObjectKind::Sink : AudioObjectKind::Source;
        const auto *destination = snapshot.object(destinationKind, request.destinationIndex);
        if (!destination || destination->identity.isEmpty() || destination->identity != request.destinationIdentity)
            return fail(QStringLiteral("The destination audio device disappeared. Refresh the list."));
        return true;
    }
    }
    return fail(QStringLiteral("This audio action is unsupported."));
}

bool AudioTypes::sameTarget(const AudioSnapshot &expected, const AudioSnapshot &current, const AudioRequest &request)
{
    if (!expected.available || !current.available || expected.serverCookie.isEmpty() || expected.serverCookie != current.serverCookie) return false;
    const auto *before = expected.object(request.kind, request.index);
    const auto *after = current.object(request.kind, request.index);
    if (!sameIdentity(before, after)) return false;
    switch (request.action) {
    case AudioAction::SetDefault:
        return request.kind == AudioObjectKind::Sink ? expected.defaultSink == current.defaultSink : expected.defaultSource == current.defaultSource;
    case AudioAction::SetVolume:
    case AudioAction::SetChannelVolumes:
        return before->channelVolumes == after->channelVolumes && before->channelNames == after->channelNames;
    case AudioAction::SetMute: return before->mute == after->mute;
    case AudioAction::SetPort: return before->activePort == after->activePort;
    case AudioAction::SetProfile: return before->activeProfile == after->activeProfile;
    case AudioAction::MoveStream: {
        const auto kind = request.kind == AudioObjectKind::SinkInput ? AudioObjectKind::Sink : AudioObjectKind::Source;
        return before->hasRoute == after->hasRoute && before->routeIndex == after->routeIndex
            && sameIdentity(expected.object(kind, request.destinationIndex), current.object(kind, request.destinationIndex));
    }
    }
    return false;
}

bool AudioTypes::matchesRequest(const AudioSnapshot &snapshot, const AudioRequest &request)
{
    if (!snapshot.available) return false;
    const auto *object = snapshot.object(request.kind, request.index);
    if (!object || object->identity != request.identity) return false;
    switch (request.action) {
    case AudioAction::SetDefault:
        return request.kind == AudioObjectKind::Sink ? snapshot.defaultSink == object->name : snapshot.defaultSource == object->name;
    case AudioAction::SetVolume: return object->volumePercent() == request.volumePercent;
    case AudioAction::SetChannelVolumes: return sameVolumes(request.channelVolumes, object->channelVolumes);
    case AudioAction::SetMute: return object->mute == request.mute;
    case AudioAction::SetPort: return object->activePort == request.choice;
    case AudioAction::SetProfile: return object->activeProfile == request.choice;
    case AudioAction::MoveStream: {
        const auto kind = request.kind == AudioObjectKind::SinkInput ? AudioObjectKind::Sink : AudioObjectKind::Source;
        const auto *destination = snapshot.object(kind, request.destinationIndex);
        return destination && destination->identity == request.destinationIdentity && object->hasRoute && object->routeIndex == request.destinationIndex;
    }
    }
    return false;
}

AudioSettings *AudioSettings::instance()
{
    static QPointer<AudioSettings> settings;
    if (!settings) settings = new AudioSettings(new PactlAudioBackend, QCoreApplication::instance());
    return settings;
}

AudioSettings::AudioSettings(AudioBackend *backend, QObject *parent) : QObject(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    qRegisterMetaType<AudioSnapshot>(); qRegisterMetaType<AudioOutcome>(); qRegisterMetaType<AudioRequest>();
    backend->setParent(this);
    connect(backend, &AudioBackend::externalChange, this, [this] {
        if (m_busy) m_refreshPending = true; else refresh();
    });
    connect(backend, &AudioBackend::readFinished, this,
        [this](quint64 token, bool success, const AudioSnapshot &snapshot, const QString &error) {
        if (token != m_token || !m_busy || m_writing) return;
        m_snapshot = snapshot;
        if (success && m_readFailed) m_error.clear();
        m_readFailed = !success;
        if (success && !m_lockedCookie.isEmpty()) {
            if (snapshot.serverCookie == m_lockedCookie) {
                m_snapshot.writable = false;
                m_snapshot.unavailableReason = m_lockReason;
            } else if (!snapshot.serverCookie.isEmpty()) { m_lockedCookie.clear(); m_lockReason.clear(); }
        }
        if (!success) {
            m_snapshot.available = false; m_snapshot.writable = false;
            m_error = error.isEmpty() ? QStringLiteral("The audio server could not be read.") : error;
        }
        busy(false);
        emit stateChanged();
        if (m_refreshPending) { m_refreshPending = false; QTimer::singleShot(0, this, &AudioSettings::refresh); }
    });
    connect(backend, &AudioBackend::writeFinished, this,
        [this](quint64 token, AudioOutcome outcome, const AudioSnapshot &snapshot, const QString &error) {
        if (token != m_token || !m_busy || !m_writing) return;
        const bool verified = outcome == AudioOutcome::Success && snapshot.serverCookie == m_snapshot.serverCookie
            && AudioTypes::matchesRequest(snapshot, m_request);
        const QString previousCookie = m_snapshot.serverCookie;
        m_snapshot = snapshot;
        m_error = verified ? QString() : !error.isEmpty() ? error : QStringLiteral("The audio change could not be verified. Refresh before trying again.");
        m_result = verified ? QStringLiteral("The audio change was applied and verified.") : QString();
        if (outcome == AudioOutcome::Uncertain || (outcome == AudioOutcome::Success && !verified)) {
            m_lockedCookie = snapshot.available && !snapshot.serverCookie.isEmpty() ? snapshot.serverCookie : previousCookie;
            m_lockReason = QStringLiteral("An audio change has an uncertain outcome. Further changes are disabled until the audio server restarts.");
            m_snapshot.writable = false;
            m_snapshot.unavailableReason = m_lockReason;
        }
        m_writing = false;
        busy(false);
        emit stateChanged();
        if (m_refreshPending) { m_refreshPending = false; QTimer::singleShot(0, this, &AudioSettings::refresh); }
    });
    QTimer::singleShot(0, this, &AudioSettings::refresh);
}

void AudioSettings::busy(bool active)
{
    m_busy = active;
    OperationManager::instance()->setActivity(this,
        m_writing ? QStringLiteral("Changing audio settings") : QStringLiteral("Reading audio settings"), active);
}

void AudioSettings::refresh()
{
    if (m_busy) { m_refreshPending = true; return; }
    m_writing = false;
    busy(true); ++m_token;
    emit stateChanged(); m_backend->readCurrent(m_token);
}

void AudioSettings::requestChange(const AudioRequest &request)
{
    if (m_busy) return;
    QString error;
    if (!AudioTypes::validateRequest(m_snapshot, request, error)) {
        m_error = error; emit stateChanged(); return;
    }
    m_request = request; m_error.clear(); m_result.clear(); m_writing = true;
    if (request.action == AudioAction::SetVolume) {
        m_request.action = AudioAction::SetChannelVolumes;
        m_request.channelVolumes = AudioTypes::scaledVolumes(*m_snapshot.object(request.kind, request.index), request.volumePercent);
    }
    busy(true); ++m_token;
    emit stateChanged(); m_backend->writeChange(m_token, m_snapshot, request);
}
