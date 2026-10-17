#include <QTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "platform/audio/AudioBackend.h"

namespace {
QJsonObject endpoint(quint32 index, const QString &name)
{
    return {{"index", double(index)}, {"name", name}, {"description", name}, {"driver", "fixture"},
        {"owner_module", 1}, {"channel_map", "front-right,front-left"}, {"mute", false},
        {"properties", QJsonObject{{"object.serial", QString::number(index + 100)}}},
        {"volume", QJsonObject{{"front-left", QJsonObject{{"value", 32768}}}, {"front-right", QJsonObject{{"value", 65536}}}}},
        {"ports", QJsonArray{QJsonObject{{"name", "speaker"}, {"description", "Speakers"}, {"availability", "available"}},
                             QJsonObject{{"name", "headset"}, {"description", "Headset"}, {"availability", "availability unknown"}},
                             QJsonObject{{"name", "absent"}, {"description", "Absent"}, {"availability", "not available"}}}},
        {"active_port", "speaker"}, {"card", 3}};
}
QJsonObject stream(quint32 index, const QString &route, quint32 destination)
{
    auto object = endpoint(index, "unused"); object.remove("name"); object.remove("ports"); object.remove("active_port");
    object.remove("card"); object.insert(route, double(destination)); object.insert("client", "12");
    object.insert("properties", QJsonObject{{"object.serial", QString::number(index + 100)},
        {"application.name", "Fixture application"}, {"application.process.id", "42"}, {"media.name", "Fixture stream"}});
    return object;
}
QJsonObject fixture()
{
    return {{"info", QJsonObject{{"cookie", "1234:5678"}, {"server_name", "PulseAudio (on fixture)"},
        {"server_version", "15.0.0"}, {"default_sink_name", "sink.one"}, {"default_source_name", "source.one"}}},
        {"sinks", QJsonArray{endpoint(1, "sink.one"), endpoint(2, "sink.two")}},
        {"sources", QJsonArray{endpoint(4, "source.one"), endpoint(5, "source.two")}},
        {"cards", QJsonArray{QJsonObject{{"index", 3}, {"name", "card.one"}, {"driver", "fixture"},
            {"properties", QJsonObject{{"object.serial", "103"}}}, {"ports", QJsonObject{}},
            {"profiles", QJsonObject{{"stereo", QJsonObject{{"description", "Stereo"}, {"available", true}}},
                                     {"off", QJsonObject{{"description", "Off"}, {"available", true}}},
                                     {"absent", QJsonObject{{"description", "Absent"}, {"available", false}}}}},
            {"active_profile", "stereo"}}}},
        {"sink-inputs", QJsonArray{stream(7, "sink", 1)}},
        {"source-outputs", QJsonArray{stream(8, "source", 4)}}, {"mode", "normal"}, {"writes", 0}};
}
QByteArray json(const QJsonValue &value)
{
    return value.isObject() ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
                            : QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
}
bool parse(const QJsonObject &state, AudioSnapshot *snapshot, QString *error)
{
    return PactlAudioBackend::parseSnapshot(json(state.value("info")), json(state.value("sinks")),
        json(state.value("sources")), json(state.value("cards")), json(state.value("sink-inputs")),
        json(state.value("source-outputs")), snapshot, error);
}
const char fakeProvider[] = R"PY(#!/usr/bin/python3
import json, os, sys, time
path = os.path.join(os.path.dirname(__file__), 'state.json')
with open(path) as f: s = json.load(f)
a = sys.argv[1:]
def save():
    with open(path, 'w') as f: json.dump(s, f)
s.setdefault('log', []).append(a)
s.setdefault('servers', []).append(os.environ.get('PULSE_SERVER'))
mode = s.get('mode', 'normal')
if a[:1] == ['--format=json']:
    if mode == 'read-timeout': time.sleep(5)
    if mode == 'read-slow': time.sleep(0.15)
    if mode == 'read-fail' or s.get('failReads'): save(); sys.exit(1)
    if mode == 'oversize': save(); print('x' * 100000); sys.exit(0)
    if mode == 'invalid': save(); print('{broken'); sys.exit(0)
    if a == ['--format=json', 'info']:
        s['infoReads'] = s.get('infoReads', 0) + 1
        if mode == 'preflight-change' and s['infoReads'] == 3: s['sinks'][0]['mute'] = True
        if mode == 'preflight-replace' and s['infoReads'] == 3: s['sinks'][0]['properties']['object.serial'] = '999'
        if mode == 'bracket-restart' and s['infoReads'] == 2: s['info']['cookie'] = 'ffff:ffff'
        if mode == 'delete-before-write' and s['infoReads'] == 4: os.unlink(__file__)
        value = s['info']
    elif len(a) == 3 and a[:2] == ['--format=json', 'list']: value = s[a[2]]
    else: save(); sys.exit(91)
    save(); print(json.dumps(value)); sys.exit(0)
s['writes'] += 1
if mode == 'write-fail': save(); print('Fixture denied', file=sys.stderr); sys.exit(1)
if mode != 'noop':
    command = a[0]
    if command.startswith('set-default-'):
        key = command[len('set-default-'):]
        obj = next(x for x in s[key + 's'] if x['index'] == int(a[1]))
        s['info']['default_' + key + '_name'] = obj['name']
    elif command == 'set-card-profile':
        next(x for x in s['cards'] if x['index'] == int(a[1]))['active_profile'] = a[2]
    elif command.startswith('move-'):
        key = command[len('move-'):]
        obj = next(x for x in s[key + 's'] if x['index'] == int(a[1]))
        obj['sink' if key == 'sink-input' else 'source'] = int(a[2])
    else:
        key, action = command[4:].rsplit('-', 1)
        obj = next(x for x in s[key + 's'] if x['index'] == int(a[1]))
        if action == 'mute': obj['mute'] = a[2] == '1'
        elif action == 'port': obj['active_port'] = a[2]
        elif action == 'volume':
            for channel, value in zip(obj['channel_map'].split(','), a[2:]): obj['volume'][channel.strip()]['value'] = int(value)
        else: save(); sys.exit(92)
if mode == 'write-read-fail': s['failReads'] = True
if mode == 'write-restart': s['info']['cookie'] = 'abcd:abcd'
if mode == 'write-disappear': s['sink-inputs'] = []
save()
if mode == 'write-timeout': time.sleep(5)
if mode == 'write-crash': os.abort()
)PY";
}

class AudioBackendTest : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        qRegisterMetaType<AudioSnapshot>(); qRegisterMetaType<AudioOutcome>();
        directory = std::make_unique<QTemporaryDir>(); QVERIFY(directory->isValid());
        executable = directory->filePath("fake-pactl");
        QFile program(executable); QVERIFY(program.open(QIODevice::WriteOnly));
        QCOMPARE(program.write(fakeProvider), qint64(sizeof(fakeProvider) - 1)); program.close();
        QVERIFY(program.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        save(fixture());
    }
    void schemaAndChannelOrder()
    {
        AudioSnapshot state; QString error; QVERIFY2(parse(fixture(), &state, &error), qPrintable(error));
        QVERIFY(state.available && state.writable); QCOMPARE(state.serverCookie, "1234:5678");
        QCOMPARE(state.sinks.size(), 2); QCOMPARE(state.cards.size(), 1);
        QCOMPARE(state.sinks[0].channelNames, QStringList({"front-right", "front-left"}));
        QCOMPARE(state.sinks[0].channelVolumes, QVector<quint32>({65536, 32768}));
        QCOMPARE(state.sinkInputs[0].name, "Fixture application");
        QVERIFY(state.sinkInputs[0].hasRoute); QCOMPARE(state.sinkInputs[0].routeIndex, 1u);
        auto changed = fixture(); auto sinks = changed["sinks"].toArray(); auto sink = sinks[0].toObject();
        sink["mute"] = true; sink["active_port"] = "headset"; sinks[0] = sink; changed["sinks"] = sinks;
        AudioSnapshot mutableState; QVERIFY(parse(changed, &mutableState, &error));
        QCOMPARE(state.sinks[0].identity, mutableState.sinks[0].identity);
        sink["properties"] = QJsonObject{{"object.serial", "replacement"}}; sinks[0] = sink; changed["sinks"] = sinks;
        QVERIFY(parse(changed, &mutableState, &error)); QVERIFY(state.sinks[0].identity != mutableState.sinks[0].identity);
    }
    void rejectsBrokenSchema_data()
    {
        QTest::addColumn<QString>("problem");
        for (const QString &problem : {"cookie", "duplicate", "index", "mute", "channel", "volume", "profile", "properties"})
            QTest::newRow(qPrintable(problem)) << problem;
    }
    void rejectsBrokenSchema()
    {
        QFETCH(QString, problem); auto state = fixture();
        auto sinks = state["sinks"].toArray(); auto sink = sinks[0].toObject();
        if (problem == "cookie") { auto info = state["info"].toObject(); info.remove("cookie"); state["info"] = info; }
        if (problem == "duplicate") sinks.append(sink);
        if (problem == "index") sink["index"] = -1;
        if (problem == "mute") sink["mute"] = "yes";
        if (problem == "channel") sink["channel_map"] = "front-right,front-right";
        if (problem == "volume") sink["volume"] = QJsonObject{{"front-left", QJsonObject{{"value", 0.5}}}};
        if (problem == "properties") sink["properties"] = QJsonArray{};
        if (problem == "profile") { auto cards = state["cards"].toArray(); auto card = cards[0].toObject(); card["profiles"] = QJsonArray{1}; cards[0] = card; state["cards"] = cards; }
        sinks[0] = sink; state["sinks"] = sinks;
        AudioSnapshot parsed; QString error; QVERIFY(!parse(state, &parsed, &error)); QVERIFY(!error.isEmpty()); QVERIFY(!parsed.available);
    }
    void unavailableStreamVolumeIsReadOnly()
    {
        auto state = fixture(); auto streams = state["source-outputs"].toArray(); auto object = streams[0].toObject();
        object["volume"] = QJsonObject{{"error", "(invalid)"}}; streams[0] = object; state["source-outputs"] = streams;
        AudioSnapshot parsed; QString error; QVERIFY(parse(state, &parsed, &error));
        QVERIFY(!parsed.sourceOutputs[0].volumeWritable); QVERIFY(parsed.sourceOutputs[0].channelVolumes.isEmpty());
    }
    void pipeWireRequiresLifetimeSerial()
    {
        auto state = fixture(); auto sinks = state["sinks"].toArray(); auto sink = sinks[0].toObject();
        sink["properties"] = QJsonObject{}; sinks[0] = sink; state["sinks"] = sinks;
        auto info = state["info"].toObject(); info["server_name"] = "PulseAudio (on PipeWire 1.6.2)"; state["info"] = info;
        AudioSnapshot parsed; QString error; QVERIFY(parse(state, &parsed, &error));
        QVERIFY(parsed.available && parsed.writable); QVERIFY(parsed.sinks[0].identity.isEmpty());
        QVERIFY(!parsed.sinks[1].identity.isEmpty()); QVERIFY(!parsed.unavailableReason.isEmpty());
        QVERIFY(!AudioTypes::validateRequest(parsed, makeRequest(parsed, AudioObjectKind::Sink, AudioAction::SetMute), error));
        info["server_name"] = "pulseaudio"; state["info"] = info;
        QVERIFY(parse(state, &parsed, &error)); QVERIFY(!parsed.sinks[0].identity.isEmpty());
        QVERIFY(AudioTypes::validateRequest(parsed, makeRequest(parsed, AudioObjectKind::Sink, AudioAction::SetMute), error));
    }
    void verifiedCommands_data()
    {
        QTest::addColumn<int>("kind"); QTest::addColumn<int>("action");
        for (auto kind : {AudioObjectKind::Sink, AudioObjectKind::Source, AudioObjectKind::SinkInput, AudioObjectKind::SourceOutput}) {
            for (auto action : {AudioAction::SetVolume, AudioAction::SetMute, AudioAction::SetChannelVolumes})
                QTest::newRow(qPrintable(QString("%1-%2").arg(int(kind)).arg(int(action)))) << int(kind) << int(action);
        }
        for (auto kind : {AudioObjectKind::Sink, AudioObjectKind::Source}) {
            QTest::newRow(qPrintable(QString("default-%1").arg(int(kind)))) << int(kind) << int(AudioAction::SetDefault);
            QTest::newRow(qPrintable(QString("port-%1").arg(int(kind)))) << int(kind) << int(AudioAction::SetPort);
        }
        QTest::newRow("profile") << int(AudioObjectKind::Card) << int(AudioAction::SetProfile);
        QTest::newRow("move-playback") << int(AudioObjectKind::SinkInput) << int(AudioAction::MoveStream);
        QTest::newRow("move-recording") << int(AudioObjectKind::SourceOutput) << int(AudioAction::MoveStream);
    }
    void verifiedCommands()
    {
        QFETCH(int, kind); QFETCH(int, action);
        PactlAudioBackend backend(executable, options()); AudioSnapshot baseline = read(backend);
        auto request = makeRequest(baseline, AudioObjectKind(kind), AudioAction(action));
        QSignalSpy writes(&backend, &AudioBackend::writeFinished); backend.writeChange(2, baseline, request);
        QTRY_COMPARE_WITH_TIMEOUT(writes.size(), 1, 10000);
        QCOMPARE(qvariant_cast<AudioOutcome>(writes[0][1]), AudioOutcome::Success);
        const auto current = qvariant_cast<AudioSnapshot>(writes[0][2]);
        QVERIFY(AudioTypes::matchesRequest(current, request)); QCOMPARE(load()["writes"].toInt(), 1);
        const auto log = load()["log"].toArray();
        QCOMPARE(log.size(), 22); // initial read + fresh preflight + write + readback
        const auto command = log[14].toArray(); QVERIFY(!command[0].toString().contains("sh"));
        if (request.action == AudioAction::SetVolume) {
            QCOMPARE(command[2].toString(), "32768"); QCOMPARE(command[3].toString(), "16384");
        }
    }
    void rejectsStaleTargetBeforeWriting_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("changed-value") << QString("preflight-change");
        QTest::newRow("reused-index") << QString("preflight-replace");
    }
    void rejectsStaleTargetBeforeWriting()
    {
        QFETCH(QString, mode); PactlAudioBackend backend(executable, options()); const auto baseline = read(backend);
        mutateMode(mode); QSignalSpy writes(&backend, &AudioBackend::writeFinished);
        backend.writeChange(2, baseline, makeRequest(baseline, AudioObjectKind::Sink, AudioAction::SetMute));
        QTRY_COMPARE_WITH_TIMEOUT(writes.size(), 1, 10000);
        QCOMPARE(qvariant_cast<AudioOutcome>(writes[0][1]), AudioOutcome::Failed); QCOMPARE(load()["writes"].toInt(), 0);
    }
    void failuresAndUncertainty_data()
    {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("uncertain");
        QTest::newRow("denied") << QString("write-fail") << false;
        QTest::newRow("ack-without-change") << QString("noop") << false;
        QTest::newRow("failed-to-start-write") << QString("delete-before-write") << false;
        QTest::newRow("timeout-after-write") << QString("write-timeout") << true;
        QTest::newRow("unreadable-readback") << QString("write-read-fail") << true;
        QTest::newRow("server-replacement") << QString("write-restart") << true;
        QTest::newRow("disappeared-stream") << QString("write-disappear") << true;
    }
    void failuresAndUncertainty()
    {
        QFETCH(QString, mode); QFETCH(bool, uncertain);
        PactlAudioBackend backend(executable, options()); const auto baseline = read(backend); mutateMode(mode);
        const auto request = makeRequest(baseline, mode == "write-disappear" ? AudioObjectKind::SinkInput : AudioObjectKind::Sink, AudioAction::SetMute);
        QSignalSpy writes(&backend, &AudioBackend::writeFinished); backend.writeChange(2, baseline, request);
        QTRY_COMPARE_WITH_TIMEOUT(writes.size(), 1, 10000);
        QCOMPARE(qvariant_cast<AudioOutcome>(writes[0][1]), uncertain ? AudioOutcome::Uncertain : AudioOutcome::Failed);
        QVERIFY(!writes[0][3].toString().isEmpty());
        QCOMPARE(load()["writes"].toInt(), mode == "delete-before-write" ? 0 : 1);
        if (!uncertain) return;
        auto stored = load(); stored["mode"] = "normal"; stored.remove("failReads"); save(stored);
        const auto locked = read(backend); QVERIFY(locked.available); QVERIFY(!locked.writable);
        backend.writeChange(3, baseline, request); QTRY_COMPARE(writes.size(), 2);
        QCOMPARE(load()["writes"].toInt(), 1);
        stored = load(); auto info = stored["info"].toObject(); info["cookie"] = "eeee:ffff"; stored["info"] = info; save(stored);
        const auto replacement = read(backend); QVERIFY(replacement.writable);
    }
    void readProtocolBounds_data()
    {
        QTest::addColumn<QString>("mode");
        for (const QString &mode : {"invalid", "oversize", "read-timeout", "read-fail", "bracket-restart"})
            QTest::newRow(qPrintable(mode)) << mode;
    }
    void readProtocolBounds()
    {
        QFETCH(QString, mode); mutateMode(mode); auto opts = options();
        if (mode == "oversize") opts.maxOutputBytes = 1024;
        PactlAudioBackend backend(executable, opts); QSignalSpy reads(&backend, &AudioBackend::readFinished);
        backend.readCurrent(1); QTRY_COMPARE_WITH_TIMEOUT(reads.size(), 1, 5000);
        QVERIFY(!reads[0][1].toBool()); QVERIFY(!qvariant_cast<AudioSnapshot>(reads[0][2]).available);
        QVERIFY(!reads[0][3].toString().isEmpty()); QCOMPARE(load()["writes"].toInt(), 0);
    }
    void snapshotHasOverallDeadline()
    {
        mutateMode("read-slow"); auto opts = options(); opts.snapshotTimeoutMs = 300;
        PactlAudioBackend backend(executable, opts); QSignalSpy reads(&backend, &AudioBackend::readFinished);
        backend.readCurrent(1); QTRY_COMPARE_WITH_TIMEOUT(reads.size(), 1, 3000);
        QVERIFY(!reads[0][1].toBool()); QCOMPARE(load()["writes"].toInt(), 0);
    }
    void overlappingReadsAreRejectedWithoutDisruptingFirst()
    {
        PactlAudioBackend backend(executable, options()); QSignalSpy reads(&backend, &AudioBackend::readFinished);
        backend.readCurrent(1); backend.readCurrent(2); QTRY_COMPARE_WITH_TIMEOUT(reads.size(), 2, 7000);
        QCOMPARE(reads[0][0].toULongLong(), 2u); QVERIFY(!reads[0][1].toBool());
        QCOMPARE(reads[1][0].toULongLong(), 1u); QVERIFY(reads[1][1].toBool());
    }
    void missingExecutableFailsWithoutMutation()
    {
        PactlAudioBackend backend(directory->filePath("missing-provider"), options());
        QSignalSpy reads(&backend, &AudioBackend::readFinished); backend.readCurrent(1);
        QTRY_COMPARE(reads.size(), 1); QVERIFY(!reads[0][1].toBool());
    }
    void explicitServerSuppressesAutospawnAndIsPinned()
    {
        const bool existed = qEnvironmentVariableIsSet("PULSE_SERVER"); const QByteArray original = qgetenv("PULSE_SERVER");
        qputenv("PULSE_SERVER", "unix:/tmp/explicit-fixture-pulse");
        PactlAudioBackend backend(executable, options());
        if (existed) qputenv("PULSE_SERVER", original); else qunsetenv("PULSE_SERVER");
        const auto snapshot = read(backend); QVERIFY(snapshot.available);
        for (const auto &server : load()["servers"].toArray()) QCOMPARE(server.toString(), "unix:/tmp/explicit-fixture-pulse");
    }
    void defaultSocketIsExplicitWithoutChangingGlobalEnvironment()
    {
        const bool serverExisted = qEnvironmentVariableIsSet("PULSE_SERVER"); const QByteArray serverOriginal = qgetenv("PULSE_SERVER");
        const bool runtimeExisted = qEnvironmentVariableIsSet("PULSE_RUNTIME_PATH"); const QByteArray runtimeOriginal = qgetenv("PULSE_RUNTIME_PATH");
        qunsetenv("PULSE_SERVER"); qputenv("PULSE_RUNTIME_PATH", "/tmp/private-fixture-pulse");
        PactlAudioBackend backend(executable, options());
        if (serverExisted) qputenv("PULSE_SERVER", serverOriginal); else qunsetenv("PULSE_SERVER");
        if (runtimeExisted) qputenv("PULSE_RUNTIME_PATH", runtimeOriginal); else qunsetenv("PULSE_RUNTIME_PATH");
        const auto snapshot = read(backend); QVERIFY(snapshot.available);
        for (const auto &server : load()["servers"].toArray()) QCOMPARE(server.toString(), "unix:/tmp/private-fixture-pulse/native");
    }
private:
    PactlAudioBackend::Options options() const
    { PactlAudioBackend::Options result; result.processTimeoutMs = 500; result.snapshotTimeoutMs = 5000; result.pollIntervalMs = 0; return result; }
    void save(const QJsonObject &state)
    { QFile file(directory->filePath("state.json")); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write(QJsonDocument(state).toJson()) > 0); }
    QJsonObject load() const
    { QFile file(directory->filePath("state.json")); if (!file.open(QIODevice::ReadOnly)) return {}; return QJsonDocument::fromJson(file.readAll()).object(); }
    void mutateMode(const QString &mode)
    { auto state = load(); state["mode"] = mode; save(state); }
    AudioSnapshot read(PactlAudioBackend &backend)
    {
        QSignalSpy reads(&backend, &AudioBackend::readFinished); backend.readCurrent(1);
        if (!reads.wait(7000) || !reads[0][1].toBool()) { QTest::qFail("Fixture snapshot failed", __FILE__, __LINE__); return {}; }
        return qvariant_cast<AudioSnapshot>(reads[0][2]);
    }
    AudioRequest makeRequest(const AudioSnapshot &snapshot, AudioObjectKind kind, AudioAction action)
    {
        AudioRequest request; request.kind = kind; request.action = action;
        const auto &objects = snapshot.objects(kind); const auto &object = action == AudioAction::SetDefault ? objects.last() : objects.first();
        request.index = object.index; request.identity = object.identity; request.volumePercent = 50; request.mute = true;
        request.channelVolumes = {20000, 10000}; request.choice = kind == AudioObjectKind::Card ? "off" : "headset";
        if (action == AudioAction::MoveStream) {
            const auto &destinations = snapshot.objects(kind == AudioObjectKind::SinkInput ? AudioObjectKind::Sink : AudioObjectKind::Source);
            request.destinationIndex = destinations.last().index; request.destinationIdentity = destinations.last().identity;
        }
        return request;
    }
    std::unique_ptr<QTemporaryDir> directory;
    QString executable;
};
QTEST_GUILESS_MAIN(AudioBackendTest)
#include "audio_backend_test.moc"
