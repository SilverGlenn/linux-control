#include "AudioBackend.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <unistd.h>

namespace {
bool uintValue(const QJsonValue &value, quint32 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number >= 4294967295.0 || std::floor(number) != number) return false;
    *result = quint32(number);
    return true;
}
bool text(const QJsonValue &value, QString *result, bool empty = false)
{
    if (!value.isString()) return false;
    *result = value.toString();
    if ((!empty && result->isEmpty()) || result->size() > 4096 || result->contains(QChar::Null)) return false;
    return true;
}
bool optionalText(const QJsonValue &value, QString *result)
{
    if (value.isNull() || value.isUndefined()) { result->clear(); return true; }
    return text(value, result, true);
}
bool document(const QByteArray &data, QJsonDocument *doc, QString *error)
{
    QJsonParseError parse;
    *doc = QJsonDocument::fromJson(data, &parse);
    if (parse.error == QJsonParseError::NoError && !doc->isNull()) return true;
    *error = QStringLiteral("Audio provider returned invalid JSON: %1").arg(parse.errorString());
    return false;
}
bool infoObject(const QByteArray &data, QJsonObject *object, QString *error)
{
    QJsonDocument doc;
    if (!document(data, &doc, error)) return false;
    if (!doc.isObject()) { *error = QStringLiteral("Audio server information is not an object."); return false; }
    *object = doc.object();
    QString cookie, name, version, sink, source;
    static const QRegularExpression cookiePattern(QStringLiteral("^[0-9a-fA-F]{4}:[0-9a-fA-F]{4}$"));
    if (!text(object->value("cookie"), &cookie) || !cookiePattern.match(cookie).hasMatch()
        || !text(object->value("server_name"), &name) || !text(object->value("server_version"), &version)
        || !object->contains("default_sink_name") || !object->contains("default_source_name")
        || !optionalText(object->value("default_sink_name"), &sink)
        || !optionalText(object->value("default_source_name"), &source)) {
        *error = QStringLiteral("Audio server information is incomplete or unsupported."); return false;
    }
    return true;
}
bool choices(const QJsonValue &value, QVector<AudioChoice> *result, bool profile)
{
    // pactl uses arrays for device ports and maps keyed by name for card profiles/ports.
    if (!value.isArray() && !value.isObject()) return false;
    QJsonArray items;
    if (value.isArray()) items = value.toArray();
    else {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!it.value().isObject()) return false;
            QJsonObject item = it.value().toObject(); item.insert("name", it.key()); items.append(item);
        }
    }
    if (items.size() > 1024) return false;
    QSet<QString> names;
    for (const QJsonValue &value : items) {
        if (!value.isObject()) return false;
        const QJsonObject item = value.toObject(); AudioChoice choice;
        if (!text(item.value("name"), &choice.name) || names.contains(choice.name)
            || !optionalText(item.value("description"), &choice.description)) return false;
        names.insert(choice.name);
        if (profile) {
            if (!item.value("available").isBool()) return false;
            choice.availability = item.value("available").toBool() ? "available" : "not available";
        } else {
            if (!text(item.value("availability"), &choice.availability)) return false;
            if (choice.availability != "available" && choice.availability != "not available"
                && choice.availability != "availability unknown") return false;
        }
        result->append(choice);
    }
    return true;
}
bool objects(const QByteArray &data, AudioObjectKind kind, const QString &cookie,
             QVector<AudioObject> *result, QString *error)
{
    QJsonDocument doc;
    if (!document(data, &doc, error)) return false;
    if (!doc.isArray() || doc.array().size() > 4096) { *error = "Unsupported audio object list."; return false; }
    const bool card = kind == AudioObjectKind::Card;
    const bool stream = kind == AudioObjectKind::SinkInput || kind == AudioObjectKind::SourceOutput;
    QSet<quint32> indices; QSet<QString> names;
    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject()) { *error = "Unsupported audio object."; return false; }
        const QJsonObject item = value.toObject(); AudioObject object; object.kind = kind;
        auto invalid = [&]() { *error = QStringLiteral("Incomplete or invalid audio object in list %1.").arg(int(kind)); return false; };
        if (!uintValue(item.value("index"), &object.index) || indices.contains(object.index)) return invalid();
        indices.insert(object.index);
        if (!stream) {
            if (!text(item.value("name"), &object.name) || names.contains(object.name)) return invalid();
            names.insert(object.name);
        }
        if (!item.value("properties").isObject()) return invalid();
        const QJsonObject properties = item.value("properties").toObject();
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            QString property;
            if (!text(it.value(), &property, true)) return invalid();
            object.properties.insert(it.key(), property);
        }
        if (!optionalText(item.value("description"), &object.description)) return invalid();
        if (object.description.isEmpty()) object.description = object.properties.value("device.description").toString();
        if (stream) {
            object.name = object.properties.value("application.name").toString();
            object.description = object.properties.value("media.name").toString();
            if (object.name.isEmpty()) object.name = object.properties.value("application.process.binary").toString();
            if (object.name.isEmpty()) object.name = QStringLiteral("Stream %1").arg(object.index);
        } else if (object.description.isEmpty()) object.description = object.name;
        if (!card) {
            if (!item.value("mute").isBool() || !item.value("volume").isObject()) return invalid();
            object.mute = item.value("mute").toBool();
            const QJsonObject volume = item.value("volume").toObject();
            if (volume.contains("error")) {
                if (!stream || volume.size() != 1 || !volume.value("error").isString()) return invalid();
                object.volumeWritable = false;
            } else {
                if (volume.isEmpty() || volume.size() > 32) return invalid();
                QString channelMap;
                if (!text(item.value("channel_map"), &channelMap)) return invalid();
                const QStringList channels = channelMap.split(',', Qt::SkipEmptyParts);
                QSet<QString> seenChannels;
                if (channels.size() != volume.size()) return invalid();
                for (const QString &entry : channels) {
                    const QString channel = entry.trimmed();
                    quint32 raw;
                    const QJsonValue channelValue = volume.value(channel);
                    if (channel.isEmpty() || seenChannels.contains(channel) || !channelValue.isObject()
                        || !uintValue(channelValue.toObject().value("value"), &raw) || raw > 0x7fffffffU) return invalid();
                    seenChannels.insert(channel);
                    object.channelNames.append(channel); object.channelVolumes.append(raw);
                }
            }
            if (item.contains("volume_writable")) {
                if (!item.value("volume_writable").isBool()) return invalid();
                object.volumeWritable = object.volumeWritable && item.value("volume_writable").toBool();
            }
            if (item.contains("has_volume")) {
                if (!item.value("has_volume").isBool()) return invalid();
                object.volumeWritable = object.volumeWritable && item.value("has_volume").toBool();
            }
            if (stream) {
                if (!uintValue(item.value(kind == AudioObjectKind::SinkInput ? "sink" : "source"), &object.routeIndex)) return invalid();
                object.hasRoute = true;
            } else {
                if (!choices(item.value("ports"), &object.ports, false)
                    || !optionalText(item.value("active_port"), &object.activePort)) return invalid();
                if (item.contains("card") && !item.value("card").isNull()) {
                    // PA_INVALID_INDEX denotes a virtual endpoint without a card.
                    if (item.value("card").toDouble(-1) != 4294967295.0) {
                        if (!uintValue(item.value("card"), &object.cardIndex)) return invalid();
                        object.hasCard = true;
                    }
                }
            }
        } else {
            object.volumeWritable = object.muteWritable = false;
            if (!choices(item.value("profiles"), &object.profiles, true)
                || !choices(item.value("ports"), &object.ports, false)
                || !optionalText(item.value("active_profile"), &object.activeProfile)) return invalid();
        }
        // Identity excludes mutable mixer state and routing. PipeWire serials
        // distinguish reused numeric indices; native PulseAudio indices are
        // lifetime scoped, further bound to client/module and the server cookie.
        QJsonObject identity;
        identity.insert("cookie", cookie); identity.insert("kind", int(kind));
        identity.insert("index", double(object.index));
        for (const QString &key : {QStringLiteral("name"), QStringLiteral("driver"), QStringLiteral("owner_module"),
                                  QStringLiteral("client"), QStringLiteral("sample_specification"), QStringLiteral("channel_map")})
            identity.insert(key, item.value(key));
        QJsonObject stable;
        for (const QString &key : {QStringLiteral("object.serial"), QStringLiteral("node.name"), QStringLiteral("device.serial"),
                                  QStringLiteral("application.id"), QStringLiteral("application.process.id"), QStringLiteral("application.process.binary")})
            if (properties.contains(key)) stable.insert(key, properties.value(key));
        identity.insert("properties", stable);
        object.identity = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        result->append(object);
    }
    return true;
}
}

bool PactlAudioBackend::parseSnapshot(const QByteArray &info, const QByteArray &sinks,
    const QByteArray &sources, const QByteArray &cards, const QByteArray &sinkInputs,
    const QByteArray &sourceOutputs, AudioSnapshot *snapshot, QString *error)
{
    if (!snapshot || !error) return false;
    *snapshot = {}; error->clear(); QJsonObject server;
    if (!infoObject(info, &server, error)) return false;
    AudioSnapshot parsed;
    parsed.serverCookie = server.value("cookie").toString().toLower();
    parsed.serverName = server.value("server_name").toString(); parsed.serverVersion = server.value("server_version").toString();
    parsed.defaultSink = server.value("default_sink_name").toString(); parsed.defaultSource = server.value("default_source_name").toString();
    if (!objects(sinks, AudioObjectKind::Sink, parsed.serverCookie, &parsed.sinks, error)
        || !objects(sources, AudioObjectKind::Source, parsed.serverCookie, &parsed.sources, error)
        || !objects(cards, AudioObjectKind::Card, parsed.serverCookie, &parsed.cards, error)
        || !objects(sinkInputs, AudioObjectKind::SinkInput, parsed.serverCookie, &parsed.sinkInputs, error)
        || !objects(sourceOutputs, AudioObjectKind::SourceOutput, parsed.serverCookie, &parsed.sourceOutputs, error)) return false;
    if (parsed.serverName.contains("PipeWire", Qt::CaseInsensitive)) {
        static const QRegularExpression serialPattern(QStringLiteral("^[0-9]+$"));
        bool missingIdentity = false;
        for (auto *group : {&parsed.sinks, &parsed.sources, &parsed.cards, &parsed.sinkInputs, &parsed.sourceOutputs}) {
            for (auto &object : *group) {
                const QString serial = object.properties.value("object.serial").toString();
                bool validNumber = false;
                const auto number = serial.toULongLong(&validNumber);
                if (!validNumber || number == 0 || !serialPattern.match(serial).hasMatch()) {
                    // PipeWire can reuse its numeric indices within one server
                    // lifetime. Keep discovery, but do not claim write identity.
                    object.identity.clear(); missingIdentity = true;
                }
            }
        }
        if (missingIdentity) parsed.unavailableReason = QStringLiteral("Some PipeWire audio objects are read-only because the provider did not expose a valid lifetime serial.");
    }
    parsed.available = parsed.writable = true;
    *snapshot = parsed; return true;
}

struct PactlAudioBackend::SnapshotRead {
    int step = 0;
    QVector<QByteArray> data;
    QElapsedTimer elapsed;
    Completion done;
};

PactlAudioBackend::PactlAudioBackend(QObject *parent)
    : PactlAudioBackend(QStringLiteral("pactl"), Options{}, parent) {}
PactlAudioBackend::PactlAudioBackend(const QString &executable, const Options &options, QObject *parent)
    : AudioBackend(parent), m_executable(executable), m_options(options)
{
    // libpulse client-conf.c disables autospawn whenever PULSE_SERVER exists.
    // Pin the destination for this backend's lifetime so a changed process
    // environment cannot redirect a mutation after its preflight.
    if (qEnvironmentVariableIsSet("PULSE_SERVER")) m_server = qEnvironmentVariable("PULSE_SERVER");
    else {
        QString runtime = qEnvironmentVariable("PULSE_RUNTIME_PATH");
        if (runtime.isEmpty() || !QDir::isAbsolutePath(runtime)) {
            runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
            if (runtime.isEmpty() || !QDir::isAbsolutePath(runtime)) runtime = QStringLiteral("/run/user/%1").arg(getuid());
            runtime = QDir(runtime).filePath("pulse");
        }
        m_server = "unix:" + QDir(runtime).filePath("native");
    }
    m_options.processTimeoutMs = qBound(20, m_options.processTimeoutMs, 30000);
    m_options.snapshotTimeoutMs = qBound(20, m_options.snapshotTimeoutMs, 120000);
    m_options.maxOutputBytes = qBound(1024, m_options.maxOutputBytes, 16 * 1024 * 1024);
    if (m_options.pollIntervalMs > 0) {
        m_poll.setInterval(qMax(1000, m_options.pollIntervalMs));
        connect(&m_poll, &QTimer::timeout, this, [this] { if (!m_busy) emit externalChange(); });
        m_poll.start();
    }
}

void PactlAudioBackend::run(const QStringList &arguments, int timeoutMs,
                           const std::function<void(const ProcessResult &)> &done)
{
    auto *process = new QProcess(this); auto *timer = new QTimer(process); timer->setSingleShot(true);
    struct State { ProcessResult result; QByteArray errors; bool finished = false; };
    auto state = std::make_shared<State>();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL", "C"); environment.insert("LANG", "C");
    environment.insert("PULSE_SERVER", m_server);
    process->setProcessEnvironment(environment);
    process->setProgram(m_executable); process->setArguments(arguments);
    auto drain = [this, process, state] {
        const QByteArray output = process->readAllStandardOutput(); const QByteArray errors = process->readAllStandardError();
        const qint64 total = qint64(state->result.output.size()) + state->errors.size() + output.size() + errors.size();
        if (total > m_options.maxOutputBytes) {
            state->result.ambiguous = true; state->result.error = tr("Audio provider exceeded its output limit."); process->kill(); return;
        }
        state->result.output += output; state->errors += errors;
    };
    auto finish = [process, timer, state, done, drain](int exitCode, QProcess::ExitStatus exitStatus) {
        if (state->finished) return;
        drain(); state->finished = true; timer->stop();
        state->result.success = state->result.error.isEmpty() && exitStatus == QProcess::NormalExit && exitCode == 0;
        if (!state->result.success && state->result.error.isEmpty()) {
            state->result.ambiguous = exitStatus == QProcess::CrashExit;
            state->result.error = QString::fromUtf8(state->errors).trimmed().left(1024);
            if (state->result.error.isEmpty()) state->result.error = QStringLiteral("Audio provider failed (exit %1).").arg(exitCode);
        }
        process->deleteLater(); done(state->result);
    };
    connect(process, &QProcess::started, this, [state] { state->result.started = true; });
    connect(process, &QProcess::readyReadStandardOutput, this, drain);
    connect(process, &QProcess::readyReadStandardError, this, drain);
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, finish);
    connect(process, &QProcess::errorOccurred, this, [process, state, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { state->result.error = process->errorString(); finish(-1, QProcess::NormalExit); }
    });
    connect(timer, &QTimer::timeout, this, [process, state] {
        state->result.ambiguous = true; state->result.error = QStringLiteral("Audio provider timed out."); process->kill();
    });
    timer->start(qMax(1, timeoutMs)); process->start();
}

void PactlAudioBackend::readSnapshot(const Completion &done)
{
    auto read = std::make_shared<SnapshotRead>(); read->done = done; read->elapsed.start(); readStep(read);
}
void PactlAudioBackend::readStep(const std::shared_ptr<SnapshotRead> &read)
{
    static const QStringList lists = {"sinks", "sources", "cards", "sink-inputs", "source-outputs"};
    const int remaining = m_options.snapshotTimeoutMs - int(read->elapsed.elapsed());
    if (remaining <= 0) { read->done(false, {}, tr("Audio snapshot timed out.")); return; }
    QStringList arguments {"--format=json"};
    if (read->step == 0 || read->step == 6) arguments << "info";
    else arguments << "list" << lists.at(read->step - 1);
    run(arguments, qMin(remaining, m_options.processTimeoutMs), [this, read](const ProcessResult &result) {
        if (!result.success) { read->done(false, {}, result.error); return; }
        if (read->elapsed.elapsed() > m_options.snapshotTimeoutMs) {
            read->done(false, {}, tr("Audio snapshot timed out.")); return;
        }
        read->data.append(result.output);
        if (++read->step < 7) { readStep(read); return; }
        QString error; QJsonObject before, after;
        if (!infoObject(read->data[0], &before, &error) || !infoObject(read->data[6], &after, &error)) {
            read->done(false, {}, error); return;
        }
        if (before.value("cookie").toString().compare(after.value("cookie").toString(), Qt::CaseInsensitive) != 0
            || before.value("server_name") != after.value("server_name")
            || before.value("server_version") != after.value("server_version")) {
            read->done(false, {}, tr("The audio server restarted while reading. Refresh and try again.")); return;
        }
        AudioSnapshot snapshot;
        // The final info captures defaults after the object reads.
        const bool ok = parseSnapshot(read->data[6], read->data[1], read->data[2], read->data[3], read->data[4], read->data[5], &snapshot, &error);
        if (ok) applyLock(&snapshot);
        read->done(ok, snapshot, error);
    });
}
void PactlAudioBackend::applyLock(AudioSnapshot *snapshot)
{
    if (!m_lockedCookie.isEmpty() && snapshot->serverCookie != m_lockedCookie) m_lockedCookie.clear();
    if (!m_lockedCookie.isEmpty()) {
        snapshot->writable = false;
        snapshot->unavailableReason = tr("An earlier audio change has an uncertain result. Further changes are disabled until a replacement audio server is verified.");
    }
}
void PactlAudioBackend::readCurrent(quint64 token)
{
    if (m_busy) {
        QTimer::singleShot(0, this, [this, token] { emit readFinished(token, false, {}, tr("Another audio operation is running.")); }); return;
    }
    m_busy = true;
    readSnapshot([this, token](bool ok, const AudioSnapshot &snapshot, const QString &error) {
        m_busy = false; emit readFinished(token, ok, snapshot, error);
    });
}
QStringList PactlAudioBackend::command(const AudioRequest &request) const
{
    QString type;
    switch (request.kind) {
    case AudioObjectKind::Sink: type = "sink"; break;
    case AudioObjectKind::Source: type = "source"; break;
    case AudioObjectKind::SinkInput: type = "sink-input"; break;
    case AudioObjectKind::SourceOutput: type = "source-output"; break;
    case AudioObjectKind::Card: type = "card"; break;
    }
    const QString index = QString::number(request.index);
    switch (request.action) {
    case AudioAction::SetDefault: return {"set-default-" + type, index};
    case AudioAction::SetMute: return {"set-" + type + "-mute", index, request.mute ? "1" : "0"};
    case AudioAction::SetPort: return {"set-" + type + "-port", index, request.choice};
    case AudioAction::SetProfile: return {"set-card-profile", index, request.choice};
    case AudioAction::MoveStream: return {"move-" + type, index, QString::number(request.destinationIndex)};
    case AudioAction::SetChannelVolumes: {
        QStringList arguments {"set-" + type + "-volume", index};
        for (quint32 volume : request.channelVolumes) arguments << QString::number(volume);
        return arguments;
    }
    case AudioAction::SetVolume: break; // Normalized before execution.
    }
    return {};
}
void PactlAudioBackend::writeChange(quint64 token, const AudioSnapshot &expected, const AudioRequest &request)
{
    QString error;
    if (m_busy || !AudioTypes::validateRequest(expected, request, error)) {
        if (m_busy) error = tr("Another audio operation is running.");
        QTimer::singleShot(0, this, [this, token, expected, error] { emit writeFinished(token, AudioOutcome::Failed, expected, error); }); return;
    }
    m_busy = true;
    readSnapshot([this, token, expected, request](bool ok, const AudioSnapshot &fresh, const QString &readError) {
        QString error;
        if (!ok || !AudioTypes::sameTarget(expected, fresh, request) || !AudioTypes::validateRequest(fresh, request, error)) {
            if (error.isEmpty()) error = ok ? tr("The selected audio state changed. Refresh and try again.") : readError;
            m_busy = false; emit writeFinished(token, AudioOutcome::Failed, fresh, error); return;
        }
        AudioRequest effective = request;
        if (request.action == AudioAction::SetVolume) {
            effective.action = AudioAction::SetChannelVolumes;
            effective.channelVolumes = AudioTypes::scaledVolumes(*fresh.object(request.kind, request.index), request.volumePercent);
        }
        QStringList arguments = command(effective);
        if (arguments.isEmpty()) { m_busy = false; emit writeFinished(token, AudioOutcome::Failed, fresh, tr("Unsupported audio action.")); return; }
        run(arguments, m_options.processTimeoutMs, [this, token, fresh, effective](const ProcessResult &written) {
            if (!written.started) {
                m_busy = false; emit writeFinished(token, AudioOutcome::Failed, fresh, written.error); return;
            }
            readSnapshot([this, token, fresh, effective, written](bool ok, const AudioSnapshot &current, const QString &readError) {
                AudioSnapshot result = current;
                AudioOutcome outcome = AudioOutcome::Failed;
                QString error = written.error;
                if (ok && !written.ambiguous && fresh.serverCookie == current.serverCookie
                    && AudioTypes::matchesRequest(current, effective)) {
                    outcome = AudioOutcome::Success; error.clear();
                } else if (!ok || written.ambiguous || fresh.serverCookie != current.serverCookie
                           || !AudioTypes::sameTarget(fresh, current, effective)) {
                    outcome = AudioOutcome::Uncertain;
                    m_lockedCookie = ok ? current.serverCookie : fresh.serverCookie;
                    if (ok) applyLock(&result);
                    else { result = {}; result.serverCookie = fresh.serverCookie; }
                    if (error.isEmpty()) error = readError.isEmpty() ? tr("The audio change could not be verified. Further writes are disabled until a replacement audio server is verified.") : readError;
                } else if (error.isEmpty()) error = tr("The audio server did not apply the requested change.");
                m_busy = false; emit writeFinished(token, outcome, result, error);
            });
        });
    });
}
