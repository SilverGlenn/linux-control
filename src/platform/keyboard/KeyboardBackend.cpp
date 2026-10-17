#include "KeyboardBackend.h"
#include <KConfig>
#include <KConfigGroup>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QXmlStreamReader>

namespace {
const QString busService = QStringLiteral("org.freedesktop.DBus");
const QString busPath = QStringLiteral("/org/freedesktop/DBus");
const QString kwinService = QStringLiteral("org.kde.KWin");
const QString keyboardService = QStringLiteral("org.kde.keyboard");
const QString keyboardInterface = QStringLiteral("org.kde.KeyboardLayouts");
const QStringList hardwareKeys = {QStringLiteral("KeyRepeat"), QStringLiteral("RepeatDelay"),
    QStringLiteral("RepeatRate"), QStringLiteral("NumLock")};
QString configPath(const QString &name)
{
    return QFileInfo(name).isAbsolute() ? name : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QLatin1Char('/') + name;
}
KConfig::OpenFlags configFlags(bool isolated, bool workspace = false) { return isolated ? KConfig::SimpleConfig : workspace ? KConfig::FullConfig : KConfig::NoGlobals; }
bool isWorkspace(const QString &key) { return key == QLatin1String("kbdLayoutChangedEnabled"); }
QString configForKey(const KeyboardBackendOptions &options, const QString &key)
{ return isWorkspace(key) ? options.workspaceConfig : hardwareKeys.contains(key) ? options.inputConfig : options.layoutConfig; }
QString groupForKey(const QString &key)
{ return isWorkspace(key) ? QStringLiteral("OSD") : hardwareKeys.contains(key) ? QStringLiteral("Keyboard") : QStringLiteral("Layout"); }
bool isHardware(const QString &key) { return hardwareKeys.contains(key); }
bool layoutInterface(const QString &source)
{
    QXmlStreamReader xml(source);
    bool inInterface = false, inMethod = false, found = false;
    QStringList output;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("interface"))
            inInterface = xml.attributes().value(QStringLiteral("name")) == keyboardInterface;
        if (inInterface && xml.isStartElement() && xml.name() == QLatin1String("method")) {
            inMethod = xml.attributes().value(QStringLiteral("name")) == QLatin1String("getLayoutsList");
            output.clear();
        }
        if (inMethod && xml.isStartElement() && xml.name() == QLatin1String("arg")) {
            if (xml.attributes().value(QStringLiteral("direction")) != QLatin1String("out")) return false;
            output << xml.attributes().value(QStringLiteral("type")).toString();
        }
        if (xml.isEndElement() && xml.name() == QLatin1String("method") && inMethod) {
            found = output == QStringList{QStringLiteral("a(sss)")}; inMethod = false;
        }
        if (xml.isEndElement() && xml.name() == QLatin1String("interface")) inInterface = false;
    }
    return found && !xml.hasError();
}
class DBusTransport final : public KeyboardTransport {
public:
    DBusTransport(const QDBusConnection &bus, QObject *parent) : KeyboardTransport(parent), m_bus(bus)
    {
        qDBusRegisterMetaType<QByteArrayList>();
        qDBusRegisterMetaType<QHash<QString, QByteArrayList>>();
        auto *watcher = new QDBusServiceWatcher(kwinService, bus,
            QDBusServiceWatcher::WatchForOwnerChange, this);
        watcher->addWatchedService(keyboardService);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] { emit changed(); });
    }
    void call(const QString &destination, const QString &path, const QString &interface,
              const QString &method, const QVariantList &args, Completion completion) override
    {
        auto message = QDBusMessage::createMethodCall(destination, path, interface, method);
        message.setArguments(args);
        auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 2000), this);
        auto done = std::make_shared<bool>(false);
        auto deliver = [pending, done, completion = std::move(completion), method] {
            if (*done) return;
            *done = true;
            KeyboardReply result;
            const auto reply = pending->reply();
            pending->deleteLater();
            result.success = reply.type() == QDBusMessage::ReplyMessage && reply.arguments().size() == 1;
            result.error = reply.errorName() + QStringLiteral(": ") + reply.errorMessage();
            if (result.success) result.value = reply.arguments().first();
            if (result.success && method == QLatin1String("getLayoutsList")) {
                if (result.value.metaType() != QMetaType::fromType<QDBusArgument>()) {
                    result.success = false; result.error = QStringLiteral("Unexpected layout-list D-Bus type.");
                } else {
                    const auto argument = qvariant_cast<QDBusArgument>(result.value);
                    if (argument.currentSignature() != QLatin1String("a(sss)")) {
                        result.success = false; result.error = QStringLiteral("Unexpected layout-list signature.");
                    } else {
                        QVariantList rows;
                        argument.beginArray();
                        while (!argument.atEnd() && rows.size() < 64) {
                            QString shortName, displayName, longName;
                            argument.beginStructure(); argument >> shortName >> displayName >> longName; argument.endStructure();
                            rows << QVariantMap{{QStringLiteral("shortName"),shortName}, {QStringLiteral("displayName"),displayName}, {QStringLiteral("longName"),longName}};
                        }
                        if (!argument.atEnd()) result.success = false;
                        argument.endArray(); result.value = rows;
                    }
                }
            }
            completion(result);
        };
        connect(pending, &QDBusPendingCallWatcher::finished, this, deliver);
        if (pending->isFinished()) QTimer::singleShot(0, this, deliver);
    }
    bool notify(const QString &owner, const QString &config, const QString &group, const QStringList &keys) override
    {
        QByteArrayList names;
        for (const auto &key : keys) names << key.toUtf8();
        // Same wire format as KConfig::Notify, directed to the audited owner.
        auto message = QDBusMessage::createTargetedSignal(owner, QLatin1Char('/') + config,
            QStringLiteral("org.kde.kconfig.notify"), QStringLiteral("ConfigChanged"));
        message << QVariant::fromValue(QHash<QString, QByteArrayList>{{group, names}});
        return m_bus.send(message);
    }
private:
    QDBusConnection m_bus;
};
struct ReadJob {
    KeyboardSnapshot value;
    std::function<void(bool,const KeyboardSnapshot &,const QString &)> completion;
    QTimer *timer = nullptr;
    bool done = false;
};
}
struct KeyboardWriteJob {
    quint64 token = 0;
    KeyboardSnapshot expected, current;
    QVariantMap requested;
    QStringList keys, saved, hardwareSaved, layoutSaved;
    int position = 0;
    QTimer *timer = nullptr;
    bool done = false;
};
KeyboardTransport *createKeyboardDBusTransport(const QDBusConnection &connection, QObject *parent)
{ return new DBusTransport(connection,parent); }
KdeKeyboardBackend::KdeKeyboardBackend(QObject *parent)
    : KdeKeyboardBackend(createKeyboardDBusTransport(QDBusConnection::sessionBus()), [] {
        KeyboardBackendOptions options;
        options.wayland = qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland");
        return options;
    }(), parent) {}
KdeKeyboardBackend::KdeKeyboardBackend(KeyboardTransport *transport, const KeyboardBackendOptions &options, QObject *parent)
    : KeyboardBackend(parent), m_transport(transport), m_options(options)
{
    if (!transport->parent()) transport->setParent(this);
    m_debounce.setSingleShot(true); m_debounce.setInterval(150);
    connect(&m_debounce, &QTimer::timeout, this, [this] { watchFiles(); emit externalChange(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(m_transport, &KeyboardTransport::changed, this, [this] { emit externalChange(); });
    watchFiles();
}
void KdeKeyboardBackend::watchFiles()
{
    for (const auto &path : {m_options.registryPath,QFileInfo(m_options.registryPath).dir().filePath(QStringLiteral("evdev.extras.xml"))}) {
        if (QFileInfo::exists(path) && !m_watcher.files().contains(path)) m_watcher.addPath(path);
        const auto directory=QFileInfo(path).absolutePath();
        if (QFileInfo::exists(directory) && !m_watcher.directories().contains(directory)) m_watcher.addPath(directory);
    }
    QStringList watchedConfigs{m_options.inputConfig,m_options.layoutConfig,m_options.workspaceConfig};
    if (!m_options.isolated) watchedConfigs << QStringLiteral("kdeglobals");
    for (const auto &config : std::as_const(watchedConfigs)) {
        QStringList paths{configPath(config)};
        if (!m_options.isolated && !QFileInfo(config).isAbsolute()) {
            for (const auto &root : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) paths << root + QLatin1Char('/') + config;
        }
        for (const auto &path : paths) {
            if (QFileInfo::exists(path) && !m_watcher.files().contains(path)) m_watcher.addPath(path);
            auto parent = QFileInfo(path).dir();
            while (!parent.exists() && parent.cdUp()) {}
            if (!m_watcher.directories().contains(parent.absolutePath())) m_watcher.addPath(parent.absolutePath());
        }
    }
}
bool KdeKeyboardBackend::readFiles(KeyboardSnapshot &state, QString &error) const
{
    if (m_options.isolated && (!QFileInfo(m_options.inputConfig).isAbsolute() || !QFileInfo(m_options.layoutConfig).isAbsolute() || !QFileInfo(m_options.workspaceConfig).isAbsolute())) {
        error = tr("Isolated keyboard configuration requires three absolute paths."); return false;
    }
    KConfig input(m_options.inputConfig,configFlags(m_options.isolated));
    KConfig layout(m_options.layoutConfig,configFlags(m_options.isolated));
    KConfig workspace(m_options.workspaceConfig,configFlags(m_options.isolated,true));
    KConfigGroup hardware(&input,QStringLiteral("Keyboard")), layouts(&layout,QStringLiteral("Layout")), osd(&workspace,QStringLiteral("OSD"));
    state.values.clear(); state.immutableKeys.clear();
    const auto defaults = KeyboardTypes::defaults();
    for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
        const auto &group = isWorkspace(it.key()) ? osd : isHardware(it.key()) ? hardware : layouts;
        const auto key = it.key().toUtf8();
        QVariant value;
        if (it.value().metaType() == QMetaType::fromType<QStringList>()) value = group.readEntry(key.constData(),it.value().toStringList());
        else if (it.value().metaType() == QMetaType::fromType<bool>()) value = group.readEntry(key.constData(),it.value().toBool());
        else if (it.value().metaType() == QMetaType::fromType<int>()) value = group.readEntry(key.constData(),it.value().toInt());
        else if (it.value().metaType() == QMetaType::fromType<double>()) value = group.readEntry(key.constData(),it.value().toDouble());
        else value = group.readEntry(key.constData(),it.value().toString());
        state.values.insert(it.key(),value);
        if (group.isImmutable() || group.isEntryImmutable(key.constData()) || !(isWorkspace(it.key()) ? workspace : isHardware(it.key()) ? input : layout).isConfigWritable(false)) state.immutableKeys.insert(it.key());
    }
    QByteArray revision;
    QDataStream stream(&revision,QIODevice::WriteOnly);
    if (m_options.wayland && !m_options.isolated) state.immutableKeys.insert(QStringLiteral("LayoutLoopCount"));
    stream << state.values << state.immutableKeys;
    // Include exact files and cascading policy bytes, catching same-value rewrites
    // and unrelated changes before each individual write.
    QStringList revisionConfigs{m_options.inputConfig,m_options.layoutConfig,m_options.workspaceConfig};
    if (!m_options.isolated) revisionConfigs << QStringLiteral("kdeglobals");
    for (const auto &config : std::as_const(revisionConfigs)) {
        QStringList paths{configPath(config)};
        if (!m_options.isolated && !QFileInfo(config).isAbsolute())
            for (const auto &root : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) paths << root + QLatin1Char('/') + config;
        paths.removeDuplicates();
        for (const auto &path : paths) { QFile file(path); stream << path; if (file.open(QIODevice::ReadOnly)) stream << file.readAll(); else stream << QByteArray(); }
    }
    state.revision = QCryptographicHash::hash(revision,QCryptographicHash::Sha256);
    state.available = true;
    return true;
}
void KdeKeyboardBackend::snapshot(Completion completion)
{
    auto job = std::make_shared<ReadJob>(); job->completion = std::move(completion);
    QString error;
    if (!readFiles(job->value,error)) { job->completion(false,job->value,error); return; }
    const auto extras = QFileInfo(m_options.registryPath).dir().filePath(QStringLiteral("evdev.extras.xml"));
    job->value.registry = KeyboardTypes::readRegistry(m_options.registryPath, QFileInfo::exists(extras) ? extras : QString());
    job->value.runtimeStatus = tr("Saved preferences read. Runtime model, variants, options, repeat, policy and loop adoption are not exposed by KWin's layout service. NumLock applies at session startup. Plasma watches plasmarc for OSD preference changes; OSD adoption is not exposed.");
    ++m_reads;
    job->timer = new QTimer(this); job->timer->setSingleShot(true);
    auto finish = [this,job](const QString &reason = QString()) {
        if (job->done) return;
        job->done = true; job->timer->stop(); job->timer->deleteLater(); --m_reads;
        job->value.reason = job->value.owner == m_uncertainOwner && !m_uncertainOwner.isEmpty()
            ? tr("The preceding keyboard write has an uncertain outcome for this compositor owner. Start a new desktop session before another Apply.") : reason;
        job->value.writable = job->value.reloadAvailable && job->value.registry.valid && m_options.wayland && job->value.owner != m_uncertainOwner;
        job->completion(true,job->value,QString());
    };
    connect(job->timer,&QTimer::timeout,this,[finish] { finish(QObject::tr("The keyboard runtime query timed out; saved preferences remain readable.")); });
    job->timer->start(qMax(1,m_options.readBudgetMs));
    m_transport->call(busService,busPath,busService,QStringLiteral("GetNameOwner"),{kwinService},[this,job,finish](const KeyboardReply &owner) {
        if (job->done) return;
        if (!owner.success || !owner.value.toString().startsWith(QLatin1Char(':'))) { finish(tr("KWin is unavailable; saved preferences remain readable.")); return; }
        job->value.owner = owner.value.toString();
        m_transport->call(job->value.owner,QStringLiteral("/KWin"),kwinService,QStringLiteral("supportInformation"),{},[this,job,finish](const KeyboardReply &report) {
            if (job->done) return;
            static const QRegularExpression version(QStringLiteral("(?:^|\\n)KWin version: ([^\\r\\n]+)"));
            job->value.version = version.match(report.value.toString()).captured(1).trimmed();
            if (!report.success || job->value.version != QLatin1String("6.6.6") || !m_options.wayland) { finish(tr("Keyboard editing requires the audited KWin 6.6.6 Wayland session.")); return; }
            m_transport->call(busService,busPath,busService,QStringLiteral("GetNameOwner"),{keyboardService},[this,job,finish](const KeyboardReply &owner) {
                if (job->done) return;
                if (!owner.success || owner.value.toString() != job->value.owner) { finish(tr("The keyboard layout service does not belong to the audited KWin owner.")); return; }
                m_transport->call(job->value.owner,QStringLiteral("/Layouts"),QStringLiteral("org.freedesktop.DBus.Introspectable"),QStringLiteral("Introspect"),{},[this,job,finish](const KeyboardReply &xml) {
                    if (job->done) return;
                    if (!xml.success || !layoutInterface(xml.value.toString())) { finish(tr("The keyboard layout service has an unsupported interface.")); return; }
                    m_transport->call(job->value.owner,QStringLiteral("/Layouts"),keyboardInterface,QStringLiteral("getLayoutsList"),{},[this,job,finish](const KeyboardReply &layouts) {
                        if (job->done) return;
                        if (!layouts.success || layouts.value.metaType() != QMetaType::fromType<QVariantList>()) { finish(tr("The current runtime keyboard layout list could not be read.")); return; }
                        for (const auto &row : layouts.value.toList()) {
                            const auto map = row.toMap();
                            job->value.runtimeLayouts.append({map.value(QStringLiteral("shortName")).toString(),map.value(QStringLiteral("displayName")).toString(),map.value(QStringLiteral("longName")).toString()});
                        }
                        // Recheck both well-known names after all pinned calls.
                        m_transport->call(busService,busPath,busService,QStringLiteral("GetNameOwner"),{kwinService},[this,job,finish](const KeyboardReply &owner) {
                            if (job->done) return;
                            if (!owner.success || owner.value.toString() != job->value.owner) { finish(tr("KWin changed during the keyboard query.")); return; }
                            m_transport->call(busService,busPath,busService,QStringLiteral("GetNameOwner"),{keyboardService},[job,finish](const KeyboardReply &owner) {
                                if (job->done) return;
                                if (!owner.success || owner.value.toString() != job->value.owner) { finish(QObject::tr("The keyboard service changed during the query.")); return; }
                                job->value.reloadAvailable = true;
                                job->value.runtimeLayoutsVerified = true;
                                finish(job->value.registry.valid ? QString() : job->value.registry.error);
                            });
                        });
                    });
                });
            });
        });
    });
}
void KdeKeyboardBackend::readCurrent(quint64 token)
{
    snapshot([this,token](bool success,const KeyboardSnapshot &state,const QString &error) { emit readFinished(token,success,state,error); });
}
void KdeKeyboardBackend::writePreferences(quint64 token,const KeyboardSnapshot &expected,const QVariantMap &requested)
{
    QString error;
    if (m_job || !expected.writable || !expected.reloadAvailable || expected.owner.isEmpty()
        || expected.version != QLatin1String("6.6.6") || !KeyboardTypes::validate(expected,requested,error)) {
        emit writeFinished(token,KeyboardOutcome::Failed,expected,error.isEmpty() ? tr("The keyboard settings cannot be written in this state.") : error); return;
    }
    auto job = std::make_shared<KeyboardWriteJob>();
    job->token=token; job->expected=expected; job->current=expected; job->requested=requested;
    job->keys=KeyboardTypes::changedKeys(expected.values,requested);
    m_job=job;
    job->timer=new QTimer(this);job->timer->setSingleShot(true);
    connect(job->timer,&QTimer::timeout,this,[this,job] { finishWrite(job,job->saved.isEmpty()?KeyboardOutcome::Failed:KeyboardOutcome::Uncertain,tr("The keyboard apply operation timed out. Read the saved values again before retrying.")); });
    job->timer->start(qMax(1,m_options.writeBudgetMs));
    nextWrite(job);
}
void KdeKeyboardBackend::nextWrite(const std::shared_ptr<KeyboardWriteJob> &job)
{
    if (job->done) return;
    snapshot([this,job](bool success,const KeyboardSnapshot &state,const QString &error) {
        if (job->done) return;
        job->current=state;
        if (!success || !state.writable || state.owner!=job->expected.owner || state.version!=job->expected.version
            || state.revision!=job->expected.revision || state.registry.revision!=job->expected.registry.revision) {
            finishWrite(job,job->saved.isEmpty()?KeyboardOutcome::Failed:KeyboardOutcome::Partial,error.isEmpty()?tr("Keyboard preferences, policy, registry or compositor changed before a write. Refresh and review the saved values."):error);return;
        }
        if (job->position>=job->keys.size()) {
            bool sent=true;
            if (!job->hardwareSaved.isEmpty()) sent=m_transport->notify(state.owner,QStringLiteral("kcminputrc"),QStringLiteral("Keyboard"),job->hardwareSaved)&&sent;
            if (!job->layoutSaved.isEmpty()) sent=m_transport->notify(state.owner,QStringLiteral("kxkbrc"),QStringLiteral("Layout"),job->layoutSaved)&&sent;
            if (!sent) { finishWrite(job,KeyboardOutcome::Uncertain,tr("Preferences were saved and read back, but the native keyboard reload notification could not be sent."));return; }
            // Give KConfigWatcher an event-loop turn; this is observation only.
            QTimer::singleShot(100,this,[this,job] {
                snapshot([this,job](bool success,const KeyboardSnapshot &observed,const QString &error) {
                    if (job->done) return;
                    job->current=observed;
                    if (!success || observed.owner!=job->expected.owner || observed.version!=job->expected.version
                        || observed.revision!=job->expected.revision || !observed.reloadAvailable) {
                        finishWrite(job,KeyboardOutcome::Uncertain,error.isEmpty()?tr("Preferences were saved, but the compositor or configuration changed during reload observation."):error);return;
                    }
                    job->current.runtimeStatus=tr("Preferences saved and read back; native KConfig reload notification sent. Plasma watches plasmarc for OSD preference changes; OSD runtime adoption is not exposed. The current layout list was observed. KWin does not expose verification of model, variants, options, repeat, policy or loop adoption. NumLock applies at next session startup; layout loop subsets are not implemented by KWin 6.6.6 Wayland.");
                    finishWrite(job,KeyboardOutcome::Success,QString());
                });
            });
            return;
        }
        const auto key=job->keys.at(job->position);
        // Reopen directly before mutation to detect filesystem/policy changes in
        // the gap since the asynchronous compositor/registry baseline query.
        KeyboardSnapshot immediate=state;QString fileError;
        if (!readFiles(immediate,fileError)||immediate.revision!=state.revision||immediate.immutableKeys.contains(key)) {
            finishWrite(job,job->saved.isEmpty()?KeyboardOutcome::Failed:KeyboardOutcome::Partial,tr("Keyboard configuration changed immediately before writing %1.").arg(key));return;
        }
        KConfig config(configForKey(m_options,key),configFlags(m_options.isolated,isWorkspace(key)));
        KConfigGroup group(&config,groupForKey(key));
        if (group.isImmutable()||group.isEntryImmutable(key.toUtf8().constData())||!config.isConfigWritable(false)) {
            finishWrite(job,job->saved.isEmpty()?KeyboardOutcome::Failed:KeyboardOutcome::Partial,tr("Keyboard preference %1 is locked or its file is not writable.").arg(key));return;
        }
        const auto value=job->requested.value(key);const auto keyBytes=key.toUtf8();
        if (value.metaType()==QMetaType::fromType<QStringList>()) {
            auto list=value.toStringList();if(list.size()==1&&list.first().isEmpty())list.clear();
            group.writeEntry(keyBytes.constData(),list,KConfig::Persistent);
        } else if(value.metaType()==QMetaType::fromType<bool>()) group.writeEntry(keyBytes.constData(),value.toBool(),KConfig::Persistent);
        else if(value.metaType()==QMetaType::fromType<int>()) group.writeEntry(keyBytes.constData(),value.toInt(),KConfig::Persistent);
        else if(value.metaType()==QMetaType::fromType<double>()) group.writeEntry(keyBytes.constData(),value.toDouble(),KConfig::Persistent);
        else group.writeEntry(keyBytes.constData(),value.toString(),KConfig::Persistent);
        if(!(m_options.isolated&&m_options.syncForTest?m_options.syncForTest(config):config.sync())) {
            // finishWrite performs readback and emits synchronously. Discard the
            // unsaved dirty entries first so neither it nor destruction retries.
            config.markAsClean();
            finishWrite(job,KeyboardOutcome::Uncertain,tr("Saving %1 failed; the configuration file may have been partially updated.").arg(key));return;
        }
        KeyboardSnapshot readback=state;
        if(!readFiles(readback,fileError)||!KeyboardTypes::sameValues(QVariantMap{{key,readback.values.value(key)}},QVariantMap{{key,value}})) {
            job->current=readback;finishWrite(job,KeyboardOutcome::Uncertain,tr("The saved %1 value could not be verified by readback.").arg(key));return;
        }
        auto expectedValues=state.values;expectedValues.insert(key,value);
        if(!KeyboardTypes::sameValues(expectedValues,readback.values)||state.immutableKeys!=readback.immutableKeys) {
            job->current=readback;finishWrite(job,KeyboardOutcome::Uncertain,tr("Other keyboard preferences or policy changed while saving %1.").arg(key));return;
        }
        job->saved<<key;
        if (!isWorkspace(key)) (isHardware(key)?job->hardwareSaved:job->layoutSaved)<<key;
        job->current=readback;job->expected=readback;++job->position;
        QTimer::singleShot(0,this,[this,job]{nextWrite(job);});
    });
}
void KdeKeyboardBackend::finishWrite(const std::shared_ptr<KeyboardWriteJob> &job,KeyboardOutcome outcome,const QString &error)
{
    if(job->done)return;
    job->done=true;job->timer->stop();job->timer->deleteLater();
    if(outcome==KeyboardOutcome::Uncertain) {
        m_uncertainOwner=job->expected.owner;
        job->current.writable=false;
        job->current.reason=tr("The preceding keyboard write has an uncertain outcome for this compositor owner. Start a new desktop session before another Apply.");
    }
    // A final local read is safe even if a bounded remote query never answered.
    QString ignored;readFiles(job->current,ignored);
    if(outcome!=KeyboardOutcome::Success&&!job->saved.isEmpty()) {
        job->current.runtimeStatus=tr("Some preferences were saved. Runtime adoption has not been verified; refresh before retrying.");
    }
    if(m_job==job)m_job.reset();
    watchFiles();emit writeFinished(job->token,outcome,job->current,error);
}
