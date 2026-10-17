#include "InputBackend.h"
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <algorithm>
#include <memory>

namespace {
const QString service = QStringLiteral("org.kde.KWin");
const QString rootPath = QStringLiteral("/org/kde/KWin/InputDevice");
const QString manager = QStringLiteral("org.kde.KWin.InputDeviceManager");
const QString deviceInterface = QStringLiteral("org.kde.KWin.InputDevice");
const QString properties = QStringLiteral("org.freedesktop.DBus.Properties");
class DBusInputTransport final : public InputTransport {
public:
    DBusInputTransport(const QDBusConnection &connection, QObject *parent) : InputTransport(parent), m_bus(connection)
    {
        auto *watch = new QDBusServiceWatcher(service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watch, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString &, const QString &, const QString &) { emit topologyChanged(); });
        m_bus.connect(service, rootPath, manager, QStringLiteral("deviceAdded"), this, SLOT(topologyNotice(QString)));
        m_bus.connect(service, rootPath, manager, QStringLiteral("deviceRemoved"), this, SLOT(topologyNotice(QString)));
    }
    void call(const QString &destination, const QString &path, const QString &interface,
              const QString &method, const QVariantList &arguments, Completion completion) override
    {
        auto message = QDBusMessage::createMethodCall(destination, path, interface, method);
        message.setArguments(arguments);
        auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 1800), this);
        auto done = std::make_shared<bool>(false);
        auto once = [pending, done, completion = std::move(completion), method] {
            if (*done) return; *done = true;
            const QDBusMessage message = pending->reply();
            pending->deleteLater();
            InputReply reply;
            reply.success = message.type() == QDBusMessage::ReplyMessage;
            reply.errorName = message.errorName();
            const int expectedCount = method == QLatin1String("Set") ? 0 : 1;
            if (reply.success && message.arguments().size() != expectedCount) reply.success = false;
            if (reply.success && !message.arguments().isEmpty()) {
                reply.value = message.arguments().first();
                if (reply.value.metaType() == QMetaType::fromType<QDBusVariant>()) reply.value = qvariant_cast<QDBusVariant>(reply.value).variant();
                if (reply.value.metaType() == QMetaType::fromType<QDBusArgument>()) {
                    const auto arg = qvariant_cast<QDBusArgument>(reply.value);
                    if (arg.currentSignature() == QLatin1String("a{sv}")) reply.value = QVariant::fromValue(qdbus_cast<QVariantMap>(arg));
                    else if (arg.currentSignature() == QLatin1String("as")) reply.value = qdbus_cast<QStringList>(arg);
                    else reply.success = false;
                }
            }
            if (reply.success && method != QLatin1String("Set")) {
                const int type = reply.value.metaType().id();
                const int expectedType = method == QLatin1String("GetAll") ? QMetaType::QVariantMap
                    : method == QLatin1String("Get") ? QMetaType::QStringList : QMetaType::QString;
                if (type != expectedType) reply.success = false;
            }
            if (!reply.success && reply.errorName.isEmpty()) reply.errorName = QStringLiteral("org.freedesktop.DBus.Error.InvalidSignature");
            completion(reply);
        };
        connect(pending, &QDBusPendingCallWatcher::finished, this, once);
        if (pending->isFinished()) QTimer::singleShot(0, this, once);
    }
private:
    QDBusConnection m_bus;
};
bool validDevice(const InputDevice &device)
{
    const auto &p = device.properties;
    for (const auto *key : {"name", "sysName"}) if (p.value(QLatin1String(key)).metaType().id() != QMetaType::QString) return false;
    for (const auto *key : {"pointer", "touchpad", "isVirtual"}) if (p.value(QLatin1String(key)).metaType().id() != QMetaType::Bool) return false;
    for (const auto *key : {"vendor", "product"}) if (p.value(QLatin1String(key)).metaType().id() != QMetaType::UInt) return false;
    return p.value(QStringLiteral("sysName")).toString() == device.id && !device.name.isEmpty();
}
QStringList sysNames(const InputReply &reply)
{
    if (!reply.success || reply.value.metaType().id() != QMetaType::QStringList) return {};
    return reply.value.toStringList();
}
QVariantMap decodedMap(const InputReply &reply)
{
    QVariantMap map = reply.value.toMap();
    for (auto it = map.begin(); it != map.end(); ++it)
        if (it.value().metaType() == QMetaType::fromType<QDBusVariant>()) it.value() = qvariant_cast<QDBusVariant>(it.value()).variant();
    QSet<QString> watched{QStringLiteral("name"), QStringLiteral("sysName"), QStringLiteral("vendor"),
        QStringLiteral("product"), QStringLiteral("pointer"), QStringLiteral("touchpad"), QStringLiteral("isVirtual"),
        QStringLiteral("tabletTool"), QStringLiteral("tabletPad"), QStringLiteral("supportedButtons")};
    for (const auto &f : InputTypes::fields()) {
        watched.insert(QLatin1String(f.key));
        if (*f.support) watched.insert(QLatin1String(f.support));
        if (*f.defaultKey) watched.insert(QLatin1String(f.defaultKey));
    }
    for (auto it = map.begin(); it != map.end();) {
        if (!watched.contains(it.key())) it = map.erase(it); else ++it;
    }
    return map;
}
// Missing/mismatched protocol fields stay unavailable. Never execute schemas.
QStringList writableFields(const QString &xml)
{
    if (xml.size() > 262144) return {};
    QXmlStreamReader reader(xml); bool inDevice = false; QStringList fields;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("interface")) inDevice = reader.attributes().value(QStringLiteral("name")) == deviceInterface;
        else if (reader.isEndElement() && reader.name() == QLatin1String("interface")) inDevice = false;
        else if (inDevice && reader.isStartElement() && reader.name() == QLatin1String("property")) {
            const auto a = reader.attributes(); const QString name = a.value(QStringLiteral("name")).toString();
            const auto *f = InputTypes::field(name);
            if (!f || a.value(QStringLiteral("access")) != QLatin1String("readwrite")) continue;
            const QString expected = f->type == InputField::Boolean ? QStringLiteral("b") : f->type == InputField::Real ? QStringLiteral("d") : QStringLiteral("u");
            if (a.value(QStringLiteral("type")) == expected) fields << name;
        }
    }
    return reader.hasError() ? QStringList{} : fields;
}
bool isUncertain(const InputReply &reply)
{
    return reply.errorName == QLatin1String("org.freedesktop.DBus.Error.NoReply")
        || reply.errorName == QLatin1String("org.freedesktop.DBus.Error.Timeout")
        || reply.errorName == QLatin1String("org.freedesktop.DBus.Error.Disconnected");
}
}
struct InputReadContext {
    InputSnapshot value;
    QStringList names;
    int next = 0;
    bool done = false;
    std::function<void(bool, const InputSnapshot &, const QString &)> completion;
};
struct InputChange { QString id, key; QVariant value; };
struct InputWriteContext {
    quint64 token = 0;
    InputSnapshot expected, requested, last;
    QList<InputChange> changes;
    int next = 0, completed = 0, verified = 0;
    bool done = false, awaitingSet = false, unresolvedSet = false;
};
InputTransport *createInputDBusTransport(const QDBusConnection &connection, QObject *parent)
{ return new DBusInputTransport(connection, parent); }
KWinInputBackend::KWinInputBackend(QObject *parent)
    : KWinInputBackend(createInputDBusTransport(QDBusConnection::sessionBus()), qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland"), parent) {}
KWinInputBackend::KWinInputBackend(const QDBusConnection &connection, bool waylandSession, QObject *parent)
    : KWinInputBackend(createInputDBusTransport(connection), waylandSession, parent) {}
KWinInputBackend::KWinInputBackend(InputTransport *transport, bool waylandSession, QObject *parent, int readBudgetMs, int writeBudgetMs)
    : InputBackend(parent), m_transport(transport), m_wayland(waylandSession)
{
    Q_ASSERT(transport); transport->setParent(this);
    m_readBudgetMs = std::clamp(readBudgetMs, 10, 12000);
    m_writeBudgetMs = std::clamp(writeBudgetMs, 10, 45000);
    connect(transport, &InputTransport::topologyChanged, this, [this] { ++m_epoch; emit externalChange(); });
    m_poll.setInterval(4000);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (m_writing || m_reads) return;
        snapshot([this](bool success, const InputSnapshot &value, const QString &) {
            if (!success) { if (m_observed.available) { m_observed.available = false; emit externalChange(); } return; }
            const bool changed = !m_observed.available || !InputTypes::sameSnapshot(m_observed, value);
            m_observed = value;
            if (changed) emit externalChange();
        });
    });
    m_poll.start();
}
void KWinInputBackend::snapshot(ReadCompletion completion)
{
    ++m_reads;
    auto c = std::make_shared<InputReadContext>(); c->value.topologyEpoch = m_epoch;
    const std::weak_ptr<InputReadContext> weak = c;
    c->completion = [this, weak, completion = std::move(completion)](bool ok, const InputSnapshot &value, const QString &error) {
        const auto context = weak.lock();
        if (!context || context->done) return;
        context->done = true; --m_reads; completion(ok, value, error);
    };
    QTimer::singleShot(m_readBudgetMs, this, [c] { if (!c->done) c->completion(false, {}, QObject::tr("Input discovery exceeded its total time limit.")); });
    m_transport->call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {service}, [this, c](const InputReply &r) {
        if (c->done) return;
        if (!r.success || r.value.metaType().id() != QMetaType::QString || !r.value.toString().startsWith(QLatin1Char(':'))) {
            c->completion(false, {}, tr("KWin's input service is unavailable.")); return;
        }
        c->value.owner = r.value.toString();
        m_transport->call(c->value.owner, QStringLiteral("/KWin"), service, QStringLiteral("supportInformation"), {}, [this, c](const InputReply &r) {
        if (c->done) return;
            static const QRegularExpression version(QStringLiteral("(?:^|\\n)KWin version: ([^\\r\\n]+)"));
            const auto match = version.match(r.value.toString());
            c->value.version = match.hasMatch() ? match.captured(1).trimmed() : QString();
            c->value.writable = r.success && m_wayland && c->value.version == QLatin1String("6.6.6") && c->value.owner != m_uncertainOwner;
            c->value.reason = !m_wayland ? tr("Input editing requires the recorded KDE Wayland session.")
                : c->value.owner == m_uncertainOwner ? tr("An earlier write may still complete; further writes are disabled until KWin restarts.")
                : c->value.version != QLatin1String("6.6.6") ? tr("Input editing requires the audited KWin 6.6.6 protocol.") : QString();
            m_transport->call(c->value.owner, rootPath, properties, QStringLiteral("Get"), {manager, QStringLiteral("devicesSysNames")}, [this, c](const InputReply &r) {
        if (c->done) return;
                if (!r.success || r.value.metaType().id() != QMetaType::QStringList) { c->completion(false, {}, tr("KWin's input device list could not be read.")); return; }
                c->names = sysNames(r);
                static const QRegularExpression safe(QStringLiteral("^event[0-9]{1,8}$"));
                if (c->names.size() > 128 || QSet<QString>(c->names.begin(), c->names.end()).size() != c->names.size()) { c->completion(false, {}, tr("KWin returned an invalid input inventory.")); return; }
                for (const auto &id : c->names) if (!safe.match(id).hasMatch()) { c->completion(false, {}, tr("KWin returned an unsupported device identity.")); return; }
                c->names.sort(); readNextDevice(c);
            });
        });
    });
}
void KWinInputBackend::readNextDevice(const std::shared_ptr<InputReadContext> &c)
{
    if (c->done) return;
    if (c->next == c->names.size()) { finishRead(c); return; }
    const QString id = c->names[c->next++]; const QString path = rootPath + QLatin1Char('/') + id;
    m_transport->call(c->value.owner, path, properties, QStringLiteral("GetAll"), {deviceInterface}, [this, c, id, path](const InputReply &r) {
        if (c->done) return;
        if (!r.success || r.value.metaType().id() != QMetaType::QVariantMap) { c->completion(false, {}, tr("An input device disappeared or could not be read.")); return; }
        InputDevice d{id, {}, path, decodedMap(r)}; d.name = d.properties.value(QStringLiteral("name")).toString();
        if (!validDevice(d)) { c->completion(false, {}, tr("KWin returned incomplete or incompatible device properties.")); return; }
        if (!d.properties.value(QStringLiteral("pointer")).toBool() || d.properties.value(QStringLiteral("isVirtual")).toBool()
            || d.properties.value(QStringLiteral("tabletTool")).toBool() || d.properties.value(QStringLiteral("tabletPad")).toBool()) { readNextDevice(c); return; }
        m_transport->call(c->value.owner, path, QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"), {}, [this, c, d](const InputReply &r) mutable {
            if (c->done) return;
            d.properties.insert(QStringLiteral("__writable"), r.success ? writableFields(r.value.toString()) : QStringList{});
            c->value.devices << d; readNextDevice(c);
        });
    });
}
void KWinInputBackend::finishRead(const std::shared_ptr<InputReadContext> &c)
{
    if (c->done) return;
    m_transport->call(c->value.owner, rootPath, properties, QStringLiteral("Get"), {manager, QStringLiteral("devicesSysNames")}, [this, c](const InputReply &r) {
        if (c->done) return;
        auto names = sysNames(r); names.sort();
        if (!r.success || r.value.metaType().id() != QMetaType::QStringList || names != c->names || c->value.topologyEpoch != m_epoch) { c->completion(false, {}, tr("Input devices changed while being read. Refresh to retry.")); return; }
        m_transport->call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {service}, [this, c](const InputReply &r) {
        if (c->done) return;
            if (!r.success || r.value.toString() != c->value.owner || c->value.topologyEpoch != m_epoch) { c->completion(false, {}, tr("KWin restarted while input devices were being read.")); return; }
            c->value.available = true;
            if (c->value.devices.isEmpty()) c->value.reason = tr("No physical mouse or touchpad is reported by KWin.");
            c->completion(true, c->value, {});
        });
    });
}
void KWinInputBackend::readCurrent(quint64 token)
{
    snapshot([this, token](bool ok, const InputSnapshot &value, const QString &error) {
        if (ok) m_observed = value;
        emit readFinished(token, ok, value, error);
    });
}
void KWinInputBackend::writeChanges(quint64 token, const InputSnapshot &expected, const InputSnapshot &requested)
{
    auto reject = [this, token](const QString &error) { emit writeFinished(token, InputOutcome::Failed, {}, error); };
    if (m_writing || !expected.available || !expected.writable || !m_wayland || expected.version != QLatin1String("6.6.6") || expected.owner == m_uncertainOwner) { reject(tr("The current KWin input session does not permit this write.")); return; }
    if (expected.owner != requested.owner || expected.version != requested.version || expected.topologyEpoch != requested.topologyEpoch || expected.devices.size() != requested.devices.size()) { reject(tr("Input device identity changed.")); return; }
    auto c = std::make_shared<InputWriteContext>(); c->token = token; c->expected = expected; c->last = expected; c->requested = requested;
    for (const auto &before : expected.devices) {
        const auto *after = requested.device(before.id);
        if (!after || !InputTypes::sameDevice(before, *after)) { reject(tr("Input device identity changed.")); return; }
        for (auto it = before.properties.cbegin(); it != before.properties.cend(); ++it) {
            if (after->properties.value(it.key()) == it.value()) continue;
            QString error;
            if (!InputTypes::validate(before, it.key(), after->properties.value(it.key()), error)
                || !before.properties.value(QStringLiteral("__writable")).toStringList().contains(it.key())) { reject(error.isEmpty() ? tr("A requested input property is read-only.") : error); return; }
        }
        if (before.properties.keys() != after->properties.keys()) { reject(tr("Unknown input properties cannot be modified.")); return; }
        QSet<QString> grouped;
        for (const auto &f : InputTypes::fields()) {
            const QString key = QLatin1String(f.key);
            if (before.properties.value(key) == after->properties.value(key) || grouped.contains(key)) continue;
            const auto peers = InputTypes::coupledProperties(key);
            QString writeKey = key;
            if (!peers.isEmpty()) {
                for (const auto &peer : peers) { grouped.insert(peer); if (after->properties.value(peer).toBool()) writeKey = peer; }
                InputDevice projected = before;
                InputTypes::setPreference(projected, writeKey, after->properties.value(writeKey));
                for (const auto &peer : peers) if (projected.properties.value(peer) != after->properties.value(peer)) { reject(tr("The requested input modes are inconsistent.")); return; }
            }
            c->changes << InputChange{before.id, writeKey, after->properties.value(writeKey)};
        }
    }
    // Disable devices last, so other requested changes are not stranded first.
    std::stable_sort(c->changes.begin(), c->changes.end(), [](const InputChange &a, const InputChange &b) {
        return (a.key == QLatin1String("enabled") && !a.value.toBool()) < (b.key == QLatin1String("enabled") && !b.value.toBool());
    });
    m_writing = true;
    QTimer::singleShot(m_writeBudgetMs, this, [this, c] {
        if (c->done) return;
        const bool uncertain = c->awaitingSet || c->unresolvedSet || c->completed > c->verified;
        if (uncertain) m_uncertainOwner = c->expected.owner;
        finishWrite(c, uncertain ? InputOutcome::Uncertain : c->completed ? InputOutcome::Partial : InputOutcome::Failed,
                    c->last, tr("The input operation exceeded its total time limit. Remaining writes were stopped; an unresolved write disables further changes until KWin's owner changes."));
    });
    applyNext(c);
}
void KWinInputBackend::applyNext(const std::shared_ptr<InputWriteContext> &c)
{
    if (c->done) return;
    snapshot([this, c](bool ok, const InputSnapshot &current, const QString &error) {
        if (c->done) return;
        if (!ok || !current.writable || !InputTypes::sameSnapshot(c->expected, current)) {
            const bool uncertain = !ok && c->completed > c->verified;
            if (uncertain) m_uncertainOwner = c->expected.owner;
            finishWrite(c, uncertain ? InputOutcome::Uncertain : c->completed ? InputOutcome::Partial : InputOutcome::Failed, current,
                uncertain ? tr("An acknowledged input write could not be read back. Its final state is unknown; further writes are disabled until KWin's owner changes.") : ok ? tr("Input preferences, device capabilities or KWin's owner changed before the next write. Remaining changes were stopped; no rollback was attempted.") : error); return;
        }
        c->last = current; c->verified = c->completed;
        if (c->next == c->changes.size()) { finishWrite(c, InputOutcome::Success, current, {}); return; }
        const InputChange change = c->changes[c->next++]; const auto *d = current.device(change.id);
        QString validation;
        if (!d || !InputTypes::validate(*d, change.key, change.value, validation)
            || !d->properties.value(QStringLiteral("__writable")).toStringList().contains(change.key)) {
            finishWrite(c, c->completed ? InputOutcome::Partial : InputOutcome::Failed, current, tr("The input device no longer supports this change.")); return;
        }
        // Pin the unique bus owner. Read/Set is not atomic: a peer can race here.
        c->awaitingSet = true;
        m_transport->call(current.owner, d->objectPath, properties, QStringLiteral("Set"),
                          {deviceInterface, change.key, QVariant::fromValue(QDBusVariant(change.value))},
                          [this, c, current, change](const InputReply &reply) {
            if (c->done) return;
            c->awaitingSet = false;
            if (!reply.success) {
                const bool uncertain = isUncertain(reply);
                // A missing Set reply remains unresolved throughout the recovery
                // read, even after no method callback is actively awaited.
                c->unresolvedSet |= uncertain;
                if (uncertain) m_uncertainOwner = current.owner;
                snapshot([this, c, uncertain](bool, const InputSnapshot &observed, const QString &) {
                    if (c->done) return;
                    finishWrite(c, uncertain ? InputOutcome::Uncertain : c->completed ? InputOutcome::Partial : InputOutcome::Failed, observed,
                        uncertain ? tr("An input write timed out or lost its reply and may still finish. Further writes are disabled until KWin's owner changes. No speculative rollback was attempted.")
                            : tr("KWin rejected an input write. Earlier changes may already be applied; remaining changes were stopped. Revert reloads current state."));
                }); return;
            }
            ++c->completed;
            for (auto &device : c->expected.devices) if (device.id == change.id) InputTypes::setPreference(device, change.key, change.value);
            // Next iteration performs complete owner/identity/property readback.
            applyNext(c);
        });
    });
}
void KWinInputBackend::finishWrite(const std::shared_ptr<InputWriteContext> &c, InputOutcome outcome, const InputSnapshot &value, const QString &error)
{
    if (c->done) return;
    c->done = true; m_writing = false;
    if (value.available) m_observed = value;
    emit writeFinished(c->token, outcome, value, error);
}
