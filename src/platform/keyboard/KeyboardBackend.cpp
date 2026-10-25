#include "KeyboardBackend.h"
#include "operations/OperationManager.h"
#include "operations/SettingsFileReservations.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <atomic>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

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
namespace Reservations = SettingsFileReservations;
QStringList relevantFiles(const KeyboardBackendOptions &options)
{
    QStringList paths;
    for (const auto &name : {options.inputConfig, options.layoutConfig, options.workspaceConfig}) {
        paths << configPath(name);
        if (!options.isolated && !QFileInfo(name).isAbsolute())
            for (const auto &root : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) paths << root + '/' + name;
    }
    if (!options.isolated) {
        if (!qEnvironmentVariableIsSet("KDE_SKIP_KDERC")) paths << QStringLiteral("/etc/kde5rc");
        for (const auto &root : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
            paths << root + QStringLiteral("/kdeglobals") << root + QStringLiteral("/system.kdeglobals");
    }
    paths.removeDuplicates(); paths.sort(); return paths;
}
QStringList reservationPaths(const KeyboardBackendOptions &options)
{
    QStringList paths{configPath(options.inputConfig),configPath(options.layoutConfig),configPath(options.workspaceConfig)};
    if (!options.isolated) paths << configPath(QStringLiteral("kdeglobals"));
    paths.removeDuplicates(); return paths;
}
struct FileResult {
    bool success = false, attempted = false;
    KeyboardSnapshot state;
    QString error;
    QStringList keys;
    Reservations::Inspection admission;
};
// Bounded, nonblocking reads fail closed for nonregular files, dangling links
// and identity/content changes during inspection. Valid symlinks remain supported.
bool sourceImage(const QString &path,QByteArray &bytes,QByteArray &record,QString &error,bool *present=nullptr)
{
    constexpr qint64 limit=16*1024*1024;
    if(present)*present=false;
    bytes.clear();record.clear();QDataStream stream(&record,QIODevice::WriteOnly);stream<<path;
    const auto encoded=QFile::encodeName(path);struct stat linkBefore{},linkAfter{},before{},after{},named{};
    // Link count/ctime are checked while reading, but adding an alias later
    // does not change content, source identity or configuration policy.
    const auto metadata=[&stream](const struct stat &value){
        stream<<quint64(value.st_dev)<<quint64(value.st_ino)<<quint64(value.st_mode)<<quint64(value.st_uid)<<quint64(value.st_gid)
              <<qint64(value.st_size)<<qint64(value.st_mtim.tv_sec)<<qint64(value.st_mtim.tv_nsec);
    };
    const auto same=[](const struct stat &a,const struct stat &b){
        return a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_mode==b.st_mode&&a.st_uid==b.st_uid&&a.st_gid==b.st_gid
            &&a.st_nlink==b.st_nlink&&a.st_size==b.st_size&&a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec
            &&a.st_ctim.tv_sec==b.st_ctim.tv_sec&&a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;
    };
    const auto fail=[&]{error=QObject::tr("Configuration source is unreadable, nonregular, oversized or changed during inspection: %1").arg(path);return false;};
    if(::lstat(encoded.constData(),&linkBefore)!=0){
        if(errno!=ENOENT)return fail();
        if(::lstat(encoded.constData(),&linkAfter)==0||errno!=ENOENT)return fail();
        stream<<QByteArray("absent");return true;
    }
    if(present)*present=true;
    if(!S_ISREG(linkBefore.st_mode)&&!S_ISLNK(linkBefore.st_mode))return fail();
    const auto canonical=QFileInfo(path).canonicalFilePath();if(canonical.isEmpty())return fail();
    const int descriptor=::open(encoded.constData(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if(descriptor<0)return fail();
    QFile file;
    if(!file.open(descriptor,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(descriptor);return fail();}
    if(::fstat(descriptor,&before)!=0||!S_ISREG(before.st_mode)||before.st_size<0||before.st_size>limit)return fail();
    bytes=file.read(limit+1);
    if(file.error()!=QFileDevice::NoError||bytes.size()!=before.st_size||bytes.size()>limit
        ||::fstat(descriptor,&after)!=0||!same(before,after)||::stat(encoded.constData(),&named)!=0||!same(after,named)
        ||::lstat(encoded.constData(),&linkAfter)!=0||!same(linkBefore,linkAfter)||QFileInfo(path).canonicalFilePath()!=canonical)return fail();
    stream<<QByteArray("present")<<canonical<<QFileInfo(path).symLinkTarget();metadata(linkBefore);metadata(after);stream<<bytes;return true;
}
bool sourceReceipt(const KeyboardBackendOptions &options,QByteArray &receipt,QString &error)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for(const auto &path:relevantFiles(options)){
        QByteArray bytes,record;if(!sourceImage(path,bytes,record,error))return false;hash.addData(record);
    }
    receipt=hash.result();return true;
}
bool readFiles(const KeyboardBackendOptions &options, KeyboardSnapshot &state, QString &error)
{
    if (options.isolated && (!QFileInfo(options.inputConfig).isAbsolute() || !QFileInfo(options.layoutConfig).isAbsolute() || !QFileInfo(options.workspaceConfig).isAbsolute())) {
        error = QObject::tr("Isolated keyboard configuration requires three absolute paths."); return false;
    }
    QByteArray initial; if(!sourceReceipt(options,initial,error))return false;
    KConfig input(options.inputConfig,configFlags(options.isolated));
    KConfig layout(options.layoutConfig,configFlags(options.isolated));
    KConfig workspace(options.workspaceConfig,configFlags(options.isolated,true));
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
    if (options.wayland && !options.isolated) state.immutableKeys.insert(QStringLiteral("LayoutLoopCount"));
    stream << state.values << state.immutableKeys;
    // The receipt contains bounded source bytes and path/inode/metadata proof.
    stream << initial;
    QByteArray finalReceipt; if(!sourceReceipt(options,finalReceipt,error))return false;
    if(initial!=finalReceipt){error=QObject::tr("Keyboard source files changed while reading. Refresh again.");return false;}
    state.revision = QCryptographicHash::hash(revision,QCryptographicHash::Sha256);
    state.available = true;
    return true;
}

FileResult readAll(const KeyboardBackendOptions &options)
{
    FileResult result;
    if (options.isolated && options.beforeRead) options.beforeRead();
    result.success = readFiles(options,result.state,result.error);
    if (result.success) {
        const auto extras=QFileInfo(options.registryPath).dir().filePath(QStringLiteral("evdev.extras.xml"));
        QByteArray primary,extra,primaryImage,extraImage;bool extraPresent=false;
        if(!sourceImage(options.registryPath,primary,primaryImage,result.error)||!sourceImage(extras,extra,extraImage,result.error,&extraPresent)){
            result.success=false;result.state.available=false;return result;
        }
        result.state.registry=KeyboardTypes::readRegistryData(primary,extra,extraPresent);
        // Exact same bytes under a replacement inode are a new registry source.
        result.state.registry.revision=QCryptographicHash::hash(result.state.registry.revision+primaryImage+extraImage,QCryptographicHash::Sha256);
        result.state.runtimeStatus=QObject::tr("Saved preferences read. Runtime model, variants, options, repeat, policy and loop adoption are not exposed by KWin's layout service. NumLock applies at session startup. Plasma watches plasmarc for OSD preference changes; OSD adoption is not exposed.");
    }
    return result;
}
FileResult saveKey(const KeyboardBackendOptions &options,const KeyboardSnapshot &before,const QString &key,const QVariant &value,
                   const Reservations::Reservation &reservation,const QStringList &expectedKeys,
                   const std::shared_ptr<std::atomic_bool> &invalidated,const std::shared_ptr<std::atomic_bool> &cancelled)
{
    FileResult result=readAll(options);
    struct End { Reservations::Reservation lease; ~End(){Reservations::endMutation(lease);} } end{reservation};
    auto retain=[&]{result.keys << Reservations::currentKeys(reservation.paths); result.keys.removeDuplicates();};
    if (!result.success) return result;
    result.success=false;
    if (result.state.revision!=before.revision || result.state.registry.revision!=before.registry.revision
        || result.state.immutableKeys.contains(key) || Reservations::currentKeys(reservation.paths,&result.error)!=expectedKeys) {
        if(result.error.isEmpty())result.error=QObject::tr("Keyboard configuration, registry or file identity changed immediately before writing %1.").arg(key);return result;
    }
    if(invalidated->load()||cancelled->load()){result.error=QObject::tr("Keyboard saving was retired before file synchronization; no new key was written.");return result;}
    {
        KConfig config(configForKey(options,key),configFlags(options.isolated,isWorkspace(key)));
        KConfigGroup group(&config,groupForKey(key));
        if(config.isImmutable()||group.isImmutable()||group.isEntryImmutable(key.toUtf8().constData())||!config.isConfigWritable(false)) {
            result.error=QObject::tr("Keyboard preference %1 is locked or its file is not writable.").arg(key);return result;
        }
        const auto keyBytes=key.toUtf8();
        if(value.metaType()==QMetaType::fromType<QStringList>()) {
            auto list=value.toStringList();if(list.size()==1&&list.first().isEmpty())list.clear();group.writeEntry(keyBytes.constData(),list,KConfig::Persistent);
        }else if(value.metaType()==QMetaType::fromType<bool>())group.writeEntry(keyBytes.constData(),value.toBool(),KConfig::Persistent);
        else if(value.metaType()==QMetaType::fromType<int>())group.writeEntry(keyBytes.constData(),value.toInt(),KConfig::Persistent);
        else if(value.metaType()==QMetaType::fromType<double>())group.writeEntry(keyBytes.constData(),value.toDouble(),KConfig::Persistent);
        else group.writeEntry(keyBytes.constData(),value.toString(),KConfig::Persistent);
        if(options.isolated&&options.beforeSync)options.beforeSync();
        if(invalidated->load()||cancelled->load()) {config.markAsClean();result.error=QObject::tr("Keyboard saving was retired before native synchronization; no new key was written.");return result;}
        const auto boundary=readAll(options);
        if(!boundary.success||boundary.state.revision!=before.revision||boundary.state.registry.revision!=before.registry.revision
            ||Reservations::currentKeys(reservation.paths)!=expectedKeys){
            config.markAsClean();result.error=QObject::tr("Keyboard source files, registry or identities changed immediately before synchronization; no new key was written.");return result;
        }
        result.attempted=true;
        const bool synced=options.isolated&&options.syncForTest?options.syncForTest(config):config.sync();
        if(!synced){config.markAsClean();result.error=QObject::tr("Saving %1 failed; the configuration file may have been partially updated.").arg(key);}
    } // All native objects, including failed dirty objects, are destroyed before completion.
    retain();
    auto saved=readAll(options);result.state=saved.state;
    if(options.isolated&&options.afterSync)options.afterSync();
    if(!result.error.isEmpty())return result;
    if(!saved.success){result.error=saved.error;return result;}
    auto desired=before.values;desired.insert(key,value);
    if(!KeyboardTypes::sameValues(desired,saved.state.values)||before.immutableKeys!=saved.state.immutableKeys
        ||before.registry.revision!=saved.state.registry.revision) {
        result.error=QObject::tr("Keyboard preferences, registry or policy changed while saving %1. Read the saved values before retrying.").arg(key);return result;
    }
    result.success=true;return result;
}
struct ReadJob {
    KeyboardSnapshot value;
    std::function<void(bool,const KeyboardSnapshot &,const QString &)> completion;
    QTimer *timer=nullptr;
    bool done=false;
};
struct KeyboardWriteJob {
    quint64 token=0;
    KeyboardSnapshot expected,current;
    QVariantMap requested;
    QStringList keys,saved,hardwareSaved,layoutSaved,currentKeys;
    int position=0;
    QTimer *timer=nullptr;
    bool done=false,finishing=false,attempted=false;
    Reservations::Reservation reservation;
    std::shared_ptr<std::atomic_bool> invalidated=std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> cancelled=std::make_shared<std::atomic_bool>(false);
};
}

// Private GUI-thread coordinator. Native work captures values only. Its lifetime
// and activity outlive the public backend while any worker or transport call runs.
class KeyboardFileController final : public KeyboardBackend {
public:
    KeyboardFileController(KeyboardTransport *transport,const KeyboardBackendOptions &options)
        :KeyboardBackend(QCoreApplication::instance()),m_transport(transport),m_options(options)
    {
        transport->setParent(this);
        m_debounce.setSingleShot(true);m_debounce.setInterval(150);
        connect(&m_debounce,&QTimer::timeout,this,[this]{watchFiles();emit externalChange();});
        connect(&m_watcher,&QFileSystemWatcher::fileChanged,this,[this]{m_debounce.start();});
        connect(&m_watcher,&QFileSystemWatcher::directoryChanged,this,[this]{m_debounce.start();});
        connect(transport,&KeyboardTransport::changed,this,[this]{if(m_job)m_job->invalidated->store(true);emit externalChange();});
        watchFiles();
    }
    void orphan(){m_orphaned=true;retireIfIdle();}
    void readCurrent(quint64 token) override;
    void writePreferences(quint64 token,const KeyboardSnapshot &expected,const QVariantMap &requested) override;
    void cancel() override {
        if(!m_job)return;m_job->cancelled->store(true);
        emit operationPending(tr("Cancellation was requested. Keyboard saving continues until native file work completes; an already running sync was not cancelled."));
    }
private:
    using Completion=std::function<void(bool,const KeyboardSnapshot &,const QString &)>;
    template<class Work,class Done>void worker(Work work,Done done){
        auto *watcher=new QFutureWatcher<FileResult>(this);
        connect(watcher,&QFutureWatcher<FileResult>::finished,this,[this,watcher,done=std::move(done)]{auto result=watcher->result();watcher->deleteLater();done(std::move(result));});
        watcher->setFuture(QtConcurrent::run(std::move(work)));
    }
    void activity(){OperationManager::instance()->setActivity(this,m_job?tr("Saving keyboard preferences"):tr("Reading keyboard preferences"),m_job||m_reads);}
    void retireIfIdle(){if(!m_job&&!m_reads){activity();if(m_orphaned)deleteLater();}}
    void snapshot(Completion completion);
    void queryRuntime(const std::shared_ptr<ReadJob> &job);
    void nextWrite(const std::shared_ptr<KeyboardWriteJob> &job);
    void finishWrite(const std::shared_ptr<KeyboardWriteJob> &job,KeyboardOutcome outcome,const QString &error);
    void watchFiles(){
        auto paths=relevantFiles(m_options);paths<<m_options.registryPath<<QFileInfo(m_options.registryPath).dir().filePath(QStringLiteral("evdev.extras.xml"));
        for(const auto &path:paths){if(QFileInfo::exists(path)&&!m_watcher.files().contains(path))m_watcher.addPath(path);auto parent=QFileInfo(path).dir();while(!parent.exists()&&parent.cdUp()){}if(!m_watcher.directories().contains(parent.absolutePath()))m_watcher.addPath(parent.absolutePath());}
    }
    KeyboardTransport *m_transport;
    KeyboardBackendOptions m_options;
    QFileSystemWatcher m_watcher;QTimer m_debounce;
    std::shared_ptr<KeyboardWriteJob> m_job;
    int m_reads=0;bool m_orphaned=false;
    QString m_uncertainOwner;
};
void KeyboardFileController::snapshot(Completion completion)
{
    auto job=std::make_shared<ReadJob>();job->completion=std::move(completion);
    ++m_reads;activity();const auto options=m_options;
    job->timer=new QTimer(this);job->timer->setSingleShot(true);
    connect(job->timer,&QTimer::timeout,this,[this]{emit operationPending(tr("Keyboard file reading is taking longer and continues in the background."));});
    job->timer->start(qMax(1,m_options.writeBudgetMs));
    worker([options]{return readAll(options);},[this,job](FileResult result){
        job->timer->stop();job->timer->deleteLater();job->timer=nullptr;
        job->value=std::move(result.state);
        if(!result.success){--m_reads;job->completion(false,job->value,result.error);retireIfIdle();return;}
        queryRuntime(job);
    });
}
void KeyboardFileController::queryRuntime(const std::shared_ptr<ReadJob> &job)
{
    job->timer=new QTimer(this);job->timer->setSingleShot(true);
    auto finish=[this,job](const QString &reason=QString()){
        if(job->done)return;job->done=true;job->timer->stop();job->timer->deleteLater();--m_reads;
        job->value.reason=job->value.owner==m_uncertainOwner&&!m_uncertainOwner.isEmpty()?tr("The preceding keyboard write has an uncertain outcome for this compositor owner. Start a new desktop session before another Apply."):reason;
        job->value.writable=job->value.reloadAvailable&&job->value.registry.valid&&m_options.wayland&&job->value.owner!=m_uncertainOwner;
        job->completion(true,job->value,{});retireIfIdle();
    };
    connect(job->timer,&QTimer::timeout,this,[finish]{finish(QObject::tr("The keyboard runtime query timed out; saved preferences remain readable."));});
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
void KeyboardFileController::readCurrent(quint64 token)
{
    // Public reads cannot interleave with a mutation's per-key snapshots.
    if(m_job)return;
    snapshot([this,token](bool ok,const KeyboardSnapshot &state,const QString &error){emit readFinished(token,ok,state,error);});
}
void KeyboardFileController::writePreferences(quint64 token,const KeyboardSnapshot &expected,const QVariantMap &requested)
{
    QString error;
    if(m_job||m_reads||!expected.writable||!expected.reloadAvailable||expected.owner.isEmpty()
        ||expected.version!=QLatin1String("6.6.6")||!KeyboardTypes::validate(expected,requested,error)){
        emit writeFinished(token,KeyboardOutcome::Failed,expected,error.isEmpty()?tr("The keyboard settings cannot be written in this state."):error);return;
    }
    auto job=std::make_shared<KeyboardWriteJob>();job->token=token;job->expected=job->current=expected;job->requested=requested;
    job->keys=KeyboardTypes::changedKeys(expected.values,requested);m_job=job;
    job->timer=new QTimer(this);job->timer->setSingleShot(true);
    connect(job->timer,&QTimer::timeout,this,[this,job]{if(!job->done)emit operationPending(tr("Keyboard saving is taking longer and continues in the background. Wait for completion before retrying or closing."));});
    job->timer->start(qMax(1,m_options.writeBudgetMs));activity();
    const auto options=m_options;const auto inventory=Reservations::snapshot();
    worker([options,inventory]{FileResult result;result.admission=Reservations::inspect(reservationPaths(options),inventory);return result;},[this,job](FileResult result){
        QString error;job->reservation=Reservations::acquire(result.admission,error);
        if(!job->reservation.valid()){finishWrite(job,KeyboardOutcome::Failed,error);return;}
        job->currentKeys=result.admission.keys;nextWrite(job);
    });
}
void KeyboardFileController::nextWrite(const std::shared_ptr<KeyboardWriteJob> &job)
{
    if(job->done||job->finishing)return;
    snapshot([this,job](bool success,const KeyboardSnapshot &state,const QString &error){
        job->current=state;
        if(!success||!state.writable||state.owner!=job->expected.owner||state.version!=job->expected.version
            ||state.revision!=job->expected.revision||state.registry.revision!=job->expected.registry.revision
            ||job->invalidated->load()||job->cancelled->load()){
            finishWrite(job,job->saved.isEmpty()?KeyboardOutcome::Failed:KeyboardOutcome::Partial,error.isEmpty()?tr("Keyboard preferences, policy, registry, compositor or cancellation state changed before a write. Refresh and review saved values."):error);return;
        }
        if(job->position>=job->keys.size()){
            // Worker receipt + file identity check directly before the pinned notification.
            const auto options=m_options;const auto paths=job->reservation.paths;const auto keys=job->currentKeys;
            worker([options,paths,keys]{auto r=readAll(options);r.keys=Reservations::currentKeys(paths,&r.error);if(r.keys!=keys){r.success=false;if(r.error.isEmpty())r.error=QObject::tr("Keyboard file identities changed before reload.");}return r;},[this,job,state](FileResult result){
                if(!result.success||result.state.revision!=state.revision||result.state.registry.revision!=state.registry.revision
                    ||job->invalidated->load()||job->cancelled->load()){
                    finishWrite(job,KeyboardOutcome::Uncertain,tr("Keyboard preferences were saved, but their source files, provider or cancellation state changed before reload; no new notification was sent."));return;
                }
                bool sent=true;
                if(!job->hardwareSaved.isEmpty())sent=m_transport->notify(state.owner,QStringLiteral("kcminputrc"),QStringLiteral("Keyboard"),job->hardwareSaved)&&sent;
                if(!job->layoutSaved.isEmpty()&&!job->invalidated->load()&&!job->cancelled->load())sent=m_transport->notify(state.owner,QStringLiteral("kxkbrc"),QStringLiteral("Layout"),job->layoutSaved)&&sent;
                if(!sent||job->invalidated->load()||job->cancelled->load()){finishWrite(job,KeyboardOutcome::Uncertain,tr("Preferences were saved, but native reload notification failed or the provider changed during dispatch. Sent requests cannot be retracted."));return;}
                QTimer::singleShot(100,this,[this,job]{snapshot([this,job](bool success,const KeyboardSnapshot &observed,const QString &error){
                    job->current=observed;
                    if(!success||observed.owner!=job->expected.owner||observed.version!=job->expected.version||observed.revision!=job->expected.revision
                        ||observed.registry.revision!=job->expected.registry.revision||!observed.reloadAvailable||job->invalidated->load()||job->cancelled->load()){
                        finishWrite(job,KeyboardOutcome::Uncertain,error.isEmpty()?tr("Preferences were saved, but the compositor or configuration changed during reload observation."):error);return;
                    }
                    job->current.runtimeStatus=tr("Preferences saved and read back; native KConfig reload notification sent. Plasma watches plasmarc for OSD preference changes; OSD runtime adoption is not exposed. The current layout list was observed. KWin does not expose verification of model, variants, options, repeat, policy or loop adoption. NumLock applies at next session startup; layout loop subsets are not implemented by KWin 6.6.6 Wayland.");
                    finishWrite(job,KeyboardOutcome::Success,{});
                });});
            });return;
        }
        if(!Reservations::beginMutation(job->reservation)){finishWrite(job,KeyboardOutcome::Failed,tr("The keyboard reservation could not enter its native transaction."));return;}
        const auto options=m_options;const auto key=job->keys.at(job->position);const auto value=job->requested.value(key);
        const auto reservation=job->reservation;const auto keys=job->currentKeys;const auto invalidated=job->invalidated,cancelled=job->cancelled;
        worker([options,state,key,value,reservation,keys,invalidated,cancelled]{return saveKey(options,state,key,value,reservation,keys,invalidated,cancelled);},[this,job,key](FileResult result){
            job->attempted|=result.attempted;
            if(!result.keys.isEmpty()){
                if(!Reservations::refresh(job->reservation,result.keys)){result.success=false;result.error=tr("Keyboard file aliases could not be retained after synchronization.");}
                job->currentKeys=result.keys;
            }
            if(result.state.available){result.state.owner=job->current.owner;result.state.version=job->current.version;result.state.reloadAvailable=job->current.reloadAvailable;result.state.writable=job->current.writable;job->current=result.state;}
            if(!result.success){finishWrite(job,result.attempted?KeyboardOutcome::Uncertain:job->saved.isEmpty()?KeyboardOutcome::Failed:KeyboardOutcome::Partial,result.error);return;}
            job->saved<<key;if(!isWorkspace(key))(isHardware(key)?job->hardwareSaved:job->layoutSaved)<<key;
            job->expected=job->current;++job->position;QTimer::singleShot(0,this,[this,job]{nextWrite(job);});
        });
    });
}
void KeyboardFileController::finishWrite(const std::shared_ptr<KeyboardWriteJob> &job,KeyboardOutcome outcome,const QString &error)
{
    if(job->done||job->finishing)return;job->finishing=true;
    // Keep the lease/activity while final native readback and object destruction finish.
    const auto options=m_options;const auto paths=job->reservation.paths;
    worker([options,paths]{auto result=readAll(options);if(!paths.isEmpty())result.keys=Reservations::currentKeys(paths);return result;},[this,job,outcome,error](FileResult result){
        const bool retained=!job->reservation.valid()||(!result.keys.isEmpty()&&Reservations::refresh(job->reservation,result.keys));
        if(result.success){auto state=result.state;state.owner=job->current.owner;state.version=job->current.version;state.writable=job->current.writable;state.reloadAvailable=job->current.reloadAvailable;state.runtimeLayouts=job->current.runtimeLayouts;state.runtimeLayoutsVerified=job->current.runtimeLayoutsVerified;state.runtimeStatus=job->current.runtimeStatus;job->current=state;}
        auto finalOutcome=outcome;QString finalError=error;
        if(!result.success){
            job->current.available=false;job->current.writable=false;
            const auto detail=tr("Terminal keyboard readback failed; the current saved state is unavailable. %1").arg(result.error);
            finalError=finalError.isEmpty()?detail:finalError+QLatin1Char('\n')+detail;
        }
        if(outcome==KeyboardOutcome::Success&&(!retained||!result.success||result.state.revision!=job->expected.revision||!KeyboardTypes::sameValues(result.state.values,job->requested))){
            finalOutcome=KeyboardOutcome::Uncertain;
            if(finalError.isEmpty())finalError=tr("Keyboard preferences changed or could not be read at terminal verification.");
        }
        if(finalOutcome==KeyboardOutcome::Uncertain){m_uncertainOwner=job->expected.owner;job->current.writable=false;job->current.reason=tr("The preceding keyboard write has an uncertain outcome for this compositor owner. Start a new desktop session before another Apply.");}
        if(finalOutcome!=KeyboardOutcome::Success&&!job->saved.isEmpty())job->current.runtimeStatus=tr("Some preferences were saved. Runtime adoption has not been verified; refresh before retrying.");
        job->done=true;job->timer->stop();job->timer->deleteLater();Reservations::release(job->reservation);m_job.reset();watchFiles();activity();
        emit writeFinished(job->token,finalOutcome,job->current,finalError);retireIfIdle();
    });
}
KeyboardTransport *createKeyboardDBusTransport(const QDBusConnection &connection,QObject *parent){return new DBusTransport(connection,parent);}
KdeKeyboardBackend::KdeKeyboardBackend(QObject *parent)
    :KdeKeyboardBackend(createKeyboardDBusTransport(QDBusConnection::sessionBus()),[]{KeyboardBackendOptions options;options.wayland=qEnvironmentVariable("XDG_SESSION_TYPE")==QLatin1String("wayland");return options;}(),parent){}
KdeKeyboardBackend::KdeKeyboardBackend(KeyboardTransport *transport,const KeyboardBackendOptions &options,QObject *parent)
    :KeyboardBackend(parent),m_controller(new KeyboardFileController(transport,options))
{
    connect(m_controller,&KeyboardBackend::readFinished,this,&KeyboardBackend::readFinished);
    connect(m_controller,&KeyboardBackend::writeFinished,this,&KeyboardBackend::writeFinished);
    connect(m_controller,&KeyboardBackend::externalChange,this,&KeyboardBackend::externalChange);
    connect(m_controller,&KeyboardBackend::operationPending,this,&KeyboardBackend::operationPending);
}
KdeKeyboardBackend::~KdeKeyboardBackend(){m_controller->orphan();}
void KdeKeyboardBackend::readCurrent(quint64 token){m_controller->readCurrent(token);}
void KdeKeyboardBackend::writePreferences(quint64 token,const KeyboardSnapshot &expected,const QVariantMap &requested){m_controller->writePreferences(token,expected,requested);}
void KdeKeyboardBackend::cancel(){m_controller->cancel();}
