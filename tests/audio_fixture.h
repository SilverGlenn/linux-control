#pragma once
#include "settings/AudioSettings.h"

inline AudioSnapshot audioFixture()
{
    AudioSnapshot snapshot;
    snapshot.available = snapshot.writable = true;
    snapshot.serverCookie = QStringLiteral("1234:5678");
    snapshot.serverName = QStringLiteral("Fixture audio server"); snapshot.serverVersion = QStringLiteral("15.0.0");
    AudioObject sink;
    sink.kind = AudioObjectKind::Sink; sink.index = 4; sink.name = QStringLiteral("speakers");
    sink.description = QStringLiteral("Speakers"); sink.identity = QStringLiteral("stable-speakers");
    sink.channelNames = {QStringLiteral("front-left"), QStringLiteral("front-right")};
    sink.channelVolumes = {65536, 32768}; sink.hasCard = true; sink.cardIndex = 2;
    sink.ports = {{QStringLiteral("analog-output"), QStringLiteral("Speakers"), QStringLiteral("available")},
                  {QStringLiteral("headphones"), QStringLiteral("Headphones"), QStringLiteral("not available")}};
    sink.activePort = QStringLiteral("analog-output"); snapshot.sinks << sink;
    sink.index = 5; sink.name = QStringLiteral("usb"); sink.identity = QStringLiteral("stable-usb"); snapshot.sinks << sink;
    AudioObject source = sink; source.kind = AudioObjectKind::Source; source.index = 8;
    source.name = QStringLiteral("microphone"); source.identity = QStringLiteral("stable-microphone"); snapshot.sources << source;
    AudioObject card; card.kind = AudioObjectKind::Card; card.index = 2; card.name = QStringLiteral("card"); card.identity = QStringLiteral("stable-card");
    card.activeProfile = QStringLiteral("stereo"); card.profiles = {{QStringLiteral("stereo"), QStringLiteral("Stereo duplex"), QStringLiteral("available")},
        {QStringLiteral("off"), QStringLiteral("Off"), QStringLiteral("available")}}; snapshot.cards << card;
    AudioObject app = sink; app.kind = AudioObjectKind::SinkInput; app.index = 10;
    app.name = QStringLiteral("Player"); app.identity = QStringLiteral("stable-player"); app.hasRoute = true; app.routeIndex = 4;
    snapshot.sinkInputs << app;
    app.kind = AudioObjectKind::SourceOutput; app.index = 11; app.name = QStringLiteral("Recorder");
    app.identity = QStringLiteral("stable-recorder"); app.routeIndex = 8; snapshot.sourceOutputs << app;
    snapshot.defaultSink = QStringLiteral("speakers"); snapshot.defaultSource = QStringLiteral("microphone");
    return snapshot;
}

class IntegrationAudioBackend : public AudioBackend {
public:
    AudioSnapshot current = audioFixture();
    AudioRequest pending;
    quint64 pendingToken = 0;
    int writes = 0, reads = 0;
    bool failRead = false;
    void readCurrent(quint64 token) override {
        ++reads; emit readFinished(token, !failRead, failRead ? AudioSnapshot{} : current,
            failRead ? QStringLiteral("Audio read failed") : QString());
    }
    void writeChange(quint64 token, const AudioSnapshot &, const AudioRequest &request) override {
        ++writes; pending = request; pendingToken = token;
    }
    void complete(AudioOutcome outcome = AudioOutcome::Success, bool apply = true) {
        auto *object = const_cast<AudioObject *>(current.object(pending.kind, pending.index));
        if (apply && object) {
            switch (pending.action) {
            case AudioAction::SetVolume: object->channelVolumes = AudioTypes::scaledVolumes(*object, pending.volumePercent); break;
            case AudioAction::SetChannelVolumes: object->channelVolumes = pending.channelVolumes; break;
            case AudioAction::SetMute: object->mute = pending.mute; break;
            case AudioAction::SetDefault:
                if (pending.kind == AudioObjectKind::Sink) current.defaultSink = object->name; else current.defaultSource = object->name;
                break;
            case AudioAction::SetPort: object->activePort = pending.choice; break;
            case AudioAction::SetProfile: object->activeProfile = pending.choice; break;
            case AudioAction::MoveStream: object->hasRoute = true; object->routeIndex = pending.destinationIndex; break;
            }
        }
        emit writeFinished(pendingToken, outcome, current,
            outcome == AudioOutcome::Success ? QString() : QStringLiteral("Audio change failed"));
    }
};

inline AudioRequest audioVolumeRequest(const AudioSnapshot &snapshot, int percent)
{
    AudioRequest request; request.kind = AudioObjectKind::Sink; request.index = snapshot.sinks.first().index;
    request.identity = snapshot.sinks.first().identity; request.volumePercent = percent;
    return request;
}
