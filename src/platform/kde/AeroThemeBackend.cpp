#include "AeroThemeBackend.h"
#include "AeroEffectBackend.h"
#include "settings/AeroThemeContract.h"
#include "operations/OperationManager.h"
#include "operations/SettingsFileReservations.h"
#include <KConfig>
#include <KConfigGroup>
#include <KIconLoader>
#include <KIconTheme>
#include <KSharedDataCache>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

namespace {
namespace Reservations=SettingsFileReservations;
class InstalledAssets final : public AeroThemeAssetProbe {
public: bool validate(QStringList &issues) const override{return AeroThemeContract::validateInstalledAssets(issues);}
};
// Unique destinations and a service epoch keep an unsent reload away from a
// replacement KWin. Native KConfig Notify remains a broadcast, not an owner CAS.
class KdeAeroRuntime final : public AeroThemeRuntime {
public:
    explicit KdeAeroRuntime(const QDBusConnection &bus):m_bus(bus){
        auto *watcher=new QDBusServiceWatcher(QStringLiteral("org.kde.KWin"),m_bus,QDBusServiceWatcher::WatchForOwnerChange,this);
        watcher->addWatchedService(QStringLiteral("org.kde.plasmashell"));
        connect(watcher,&QDBusServiceWatcher::serviceOwnerChanged,this,[this]{++m_epoch;m_supported=false;emit externalChange();});
    }
    void probe() override {
        m_supported=false;const auto epoch=m_epoch;
        owner(QStringLiteral("org.kde.KWin"),[this,epoch](const QString &kwin){
            if(kwin.isEmpty()||epoch!=m_epoch){probeFailed();return;}m_kwin=kwin;
            owner(QStringLiteral("org.kde.plasmashell"),[this,epoch](const QString &shell){
                if(shell.isEmpty()||epoch!=m_epoch){probeFailed();return;}m_shell=shell;
                call(m_kwin,QStringLiteral("/KWin"),QStringLiteral("org.kde.KWin"),QStringLiteral("supportInformation"),{},[this,epoch](const QDBusMessage &message){
                    const QDBusReply<QString> reply(message);
                    if(epoch!=m_epoch||!reply.isValid()||!isSupportedAeroKWinReport(reply.value())){probeFailed();return;}
                    call(m_kwin,QStringLiteral("/Effects"),QStringLiteral("org.freedesktop.DBus.Introspectable"),QStringLiteral("Introspect"),{},[this,epoch](const QDBusMessage &message){
                        const QDBusReply<QString> reply(message);
                        if(epoch!=m_epoch||!reply.isValid()||!supportsEffects(reply.value())){probeFailed();return;}
                        loaded([this,epoch](bool yes){
                            if(epoch!=m_epoch||!yes){probeFailed();return;}
                            ownersMatch([this,epoch](bool same){m_supported=same&&epoch==m_epoch;emit probeFinished(m_supported,m_supported?QString():tr("The Aero runtime owner changed during discovery."));});
                        });
                    });
                });
            });
        });
    }
    void reload(const QSet<QString> &changed) override { reloadGuarded(changed,[]{return true;}); }
    void reloadGuarded(const QSet<QString> &changed,std::function<bool()> guard) override {
        if(!m_supported){emit reloadFinished(false,tr("Aero settings were saved, but their runtime became unavailable. Revert to inspect saved values."));return;}
        const auto epoch=m_epoch;
        ownersMatch([this,changed,epoch,guard=std::move(guard)](bool same){
            if(!same||epoch!=m_epoch||!guard()){emit reloadFinished(false,tr("Aero settings were saved, but the pinned runtime changed before reload; no reload was sent."));return;}
            bool dispatched=true;
            auto notify=[&](int type){auto message=QDBusMessage::createSignal(QStringLiteral("/KGlobalSettings"),QStringLiteral("org.kde.KGlobalSettings"),QStringLiteral("notifyChange"));message.setArguments({type,0});dispatched=m_bus.send(message)&&dispatched;};
            if(changed.contains(QStringLiteral("palette"))||changed.contains(QStringLiteral("colorScheme")))notify(0);
            if(changed.contains(QStringLiteral("applicationStyle"))||changed.contains(QStringLiteral("kvantumTheme")))notify(2);
            if(changed.contains(QStringLiteral("cursorTheme"))||changed.contains(QStringLiteral("cursorSize")))notify(5);
            if(changed.contains(QStringLiteral("iconTheme"))){KIconTheme::reconfigure();KSharedDataCache::deleteCache(QStringLiteral("icon-cache"));for(int i=0;i<KIconLoader::LastGroup;++i)KIconLoader::emitChange(KIconLoader::Group(i));}
            const auto reconfigure=QDBusMessage::createMethodCall(m_kwin,QStringLiteral("/KWin"),QStringLiteral("org.kde.KWin"),QStringLiteral("reconfigure"));
            dispatched=m_bus.send(reconfigure)&&dispatched;
            if(!dispatched){emit reloadFinished(false,tr("Aero settings were saved, but a KDE notification could not be dispatched. Sent requests cannot be retracted."));return;}
            call(m_kwin,QStringLiteral("/Effects"),QStringLiteral("org.kde.kwin.Effects"),QStringLiteral("reconfigureEffect"),{QStringLiteral("aeroglassblur")},[this,epoch](const QDBusMessage &message){
                const QDBusReply<void> reply(message);
                if(epoch!=m_epoch||!reply.isValid()){emit reloadFinished(false,tr("Aero settings were saved, but KWin did not acknowledge the pinned effect reload. Sent requests cannot be retracted."));return;}
                loaded([this,epoch](bool loaded){
                    if(!loaded||epoch!=m_epoch){emit reloadFinished(false,tr("Aero settings were saved, but the effect or runtime changed after reload."));return;}
                    // The retired unique owner can remain connected and answer
                    // while its name-owner watcher notification is still queued.
                    ownersMatch([this,epoch](bool same){
                        const bool current=same&&epoch==m_epoch;
                        if(!current)m_supported=false;
                        emit reloadFinished(current,current?QString():tr("Aero settings were saved, but a pinned runtime owner changed before reload verification completed. Sent reload requests cannot be retracted; runtime adoption remains uncertain. Revert to inspect saved values."));
                    });
                });
            });
        });
    }
private:
    using Reply=std::function<void(const QDBusMessage &)>;
    void call(const QString &destination,const QString &path,const QString &interface,const QString &method,const QVariantList &args,Reply done){
        auto message=QDBusMessage::createMethodCall(destination,path,interface,method);message.setArguments(args);
        auto *pending=new QDBusPendingCallWatcher(m_bus.asyncCall(message,2500),this);auto delivered=std::make_shared<bool>(false);
        auto finish=[pending,delivered,done=std::move(done)]{if(*delivered)return;*delivered=true;const auto message=pending->reply();pending->deleteLater();done(message);};
        connect(pending,&QDBusPendingCallWatcher::finished,this,finish);if(pending->isFinished())QTimer::singleShot(0,this,finish);
    }
    void owner(const QString &name,std::function<void(QString)> done){call(QStringLiteral("org.freedesktop.DBus"),QStringLiteral("/org/freedesktop/DBus"),QStringLiteral("org.freedesktop.DBus"),QStringLiteral("GetNameOwner"),{name},[done=std::move(done)](const QDBusMessage &message){const QDBusReply<QString> reply(message);done(reply.isValid()&&reply.value().startsWith(':')?reply.value():QString());});}
    void ownersMatch(std::function<void(bool)> done){const auto kwin=m_kwin,shell=m_shell;owner(QStringLiteral("org.kde.KWin"),[this,kwin,shell,done=std::move(done)](const QString &owner){if(owner!=kwin){done(false);return;}this->owner(QStringLiteral("org.kde.plasmashell"),[shell,done](const QString &owner){done(owner==shell&&!owner.isEmpty());});});}
    void loaded(std::function<void(bool)> done){call(m_kwin,QStringLiteral("/Effects"),QStringLiteral("org.kde.kwin.Effects"),QStringLiteral("isEffectLoaded"),{QStringLiteral("aeroglassblur")},[done=std::move(done)](const QDBusMessage &message){const QDBusReply<bool> reply(message);done(reply.isValid()&&reply.value());});}
    static bool supportsEffects(const QString &source){if(source.size()>65536)return false;QXmlStreamReader xml(source);bool effects=false,reconfigure=false,loaded=false;while(!xml.atEnd()){xml.readNext();if(xml.isStartElement()&&xml.name()==QLatin1String("interface"))effects=xml.attributes().value(QStringLiteral("name"))==QLatin1String("org.kde.kwin.Effects");else if(xml.isEndElement()&&xml.name()==QLatin1String("interface"))effects=false;else if(effects&&xml.isStartElement()&&xml.name()==QLatin1String("method")){const auto name=xml.attributes().value(QStringLiteral("name"));reconfigure|=name==QLatin1String("reconfigureEffect");loaded|=name==QLatin1String("isEffectLoaded");}}return !xml.hasError()&&reconfigure&&loaded;}
    void probeFailed(){emit probeFinished(false,tr("A running Plasma shell and supported loaded Aero effect on KWin 6.6.6 Wayland are required."));}
    QDBusConnection m_bus;QString m_kwin,m_shell;quint64 m_epoch=0;bool m_supported=false;
};
struct Entry { QString file, group, key; QVariant value; QString field; };
QList<Entry> entriesFor(const QVariantMap &values)
{
    QList<Entry> result;
    for (const auto &field : AeroThemeContract::fields()) {
        if (field.key != QLatin1String("palette")) {
            result.append({field.file, field.group, field.entry, values.value(field.key), field.key});
            continue;
        }
        const auto palette = values.value(field.key).toMap();
        for (auto group = palette.cbegin(); group != palette.cend(); ++group) {
            const auto map = group.value().toMap();
            for (auto item = map.cbegin(); item != map.cend(); ++item)
                result.append({field.file, group.key(), item.key(), item.value(), field.key});
        }
    }
    return result;
}
QVariant readValue(const KConfigGroup &group, const QString &key, const QVariant &type)
{
    // Identity defaults describe the approved target, never pretend a missing
    // identity marker means Aero is already selected.
    if (type.metaType().id() == QMetaType::Bool) {
        const auto raw = group.readEntry(key, type.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
        if (raw == QLatin1String("true")) return true;
        if (raw == QLatin1String("false")) return false;
        return raw;
    }
    if (type.metaType().id() == QMetaType::Int) {
        bool ok; const auto raw = group.readEntry(key, QString::number(type.toInt())); const int value = raw.toInt(&ok);
        return ok ? QVariant(value) : QVariant(raw);
    }
    // SMOD's schema defaults to the Aero resource even without an explicit
    // entry. Other component identities have different native defaults.
    const bool nativeStringDefault = key == QLatin1String("CustomColor")
        || (group.name() == QLatin1String("Windeco") && key == QLatin1String("DecorationTheme"));
    return group.readEntry(key, nativeStringDefault ? type.toString() : QString());
}
bool writable(KConfig &config, const KConfigGroup &group, const QString &key)
{
    return !config.isImmutable() && !group.isImmutable() && !group.isEntryImmutable(key) && config.isConfigWritable(false);
}
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
struct ThemeResult {
    bool success=false,attempted=false,saved=false,assetsSupported=false;
    QString error;QStringList issues,keys;
    AeroThemeState state;
    QByteArray receipt;
    Reservations::Inspection admission;
    QList<QPair<Entry,Entry>> plan;
};
struct ThemeFiles {
    QString directory;bool isolated=false;
    AeroThemeBackendOptions options;
    QStringList files;
    ThemeFiles(const QString &path,bool fixture,const AeroThemeBackendOptions &hooks):directory(path),isolated(fixture),options(hooks){
        for(const auto &field:AeroThemeContract::fields()){
            files<<directory+'/'+field.file;
            if(!isolated)for(const auto &root:QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))files<<root+'/'+field.file;
        }
        if(!isolated){if(!qEnvironmentVariableIsSet("KDE_SKIP_KDERC"))files<<QStringLiteral("/etc/kde5rc");for(const auto &root:QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))files<<root+QStringLiteral("/kdeglobals")<<root+QStringLiteral("/system.kdeglobals");}
        files.removeDuplicates();files.sort();
    }
    QString configName(const QString &name)const{return isolated?directory+'/'+name:name;}
    QStringList targets()const{QStringList paths;for(const auto &field:AeroThemeContract::fields())paths<<directory+'/'+field.file;paths.removeDuplicates();return paths;}
    bool receipt(QByteArray &out,QString &error)const{
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for(const auto &path:files){QByteArray bytes,record;if(!sourceImage(path,bytes,record,error))return false;hash.addData(record);}
        out=hash.result();return true;
    }
    bool read(ThemeResult &result)const{
    QByteArray initial; if (!receipt(initial,result.error)) return false;
    const auto target = AeroThemeContract::defaults();
    for (const auto &field : AeroThemeContract::fields()) {
        KConfig config(configName(field.file), isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
        bool canWrite = true;
        if (field.key == QLatin1String("palette")) {
            QVariantMap palette;
            const auto definitions = field.defaultValue.toMap();
            for (auto definition = definitions.cbegin(); definition != definitions.cend(); ++definition) {
                KConfigGroup group(&config, definition.key()); QVariantMap values;
                const auto entries = definition.value().toMap();
                for (auto item = entries.cbegin(); item != entries.cend(); ++item) {
                    values.insert(item.key(), readValue(group, item.key(), item.value()));
                    canWrite = writable(config, group, item.key()) && canWrite;
                }
                palette.insert(definition.key(), values);
            }
            // Capture all color/effect entries so another theme's extra groups
            // cannot remain active after restoring the approved Aero palette.
            for (const auto &name : config.groupList()) {
                if (!name.startsWith(QLatin1String("Colors:")) && !name.startsWith(QLatin1String("ColorEffects:"))) continue;
                const KConfigGroup group(&config, name);
                if (group.keyList().isEmpty() && !definitions.contains(name)) continue;
                auto values = palette.value(name).toMap();
                for (const auto &key : group.keyList()) {
                    values.insert(key, group.readEntry(key, QString()));
                    canWrite = writable(config, group, key) && canWrite;
                }
                palette.insert(name, values);
            }
            result.state.values.insert(field.key, palette);
        } else {
            const KConfigGroup group(&config, field.group);
            result.state.values.insert(field.key, readValue(group, field.entry, target.value(field.key)));
            canWrite = writable(config, group, field.entry);
        }
        if (canWrite) result.state.writableKeys.insert(field.key);
        else result.state.issues << QObject::tr("Aero setting is locked or its configuration is not writable: %1").arg(field.label);
    }
    if (!receipt(result.receipt,result.error)) return false;
    if (result.receipt != initial) { result.error = QObject::tr("Aero source files changed while reading. Refresh again."); return false; }
    return true;
    }
    QList<QPair<Entry,Entry>> prepare(const QVariantMap &before,const QVariantMap &after,QString &error)const{
    auto expected = entriesFor(before);
    auto requested = entriesFor(after);
    // Union requested + baseline palette entries. Invalid QVariant is a
    // deletion sentinel, reachable only for extra Colors:/ColorEffects: keys.
    for (const auto &before : expected) {
        const auto found = std::find_if(requested.cbegin(), requested.cend(), [&](const Entry &entry) {
            return before.file == entry.file && before.group == entry.group && before.key == entry.key;
        });
        if (found != requested.cend()) continue;
        if (before.field != QLatin1String("palette") || (!before.group.startsWith(QLatin1String("Colors:"))
            && !before.group.startsWith(QLatin1String("ColorEffects:")))) {
            error = QObject::tr("The saved Aero baseline contains an unsupported entry."); return {};
        }
        requested.append({before.file, before.group, before.key, {}, before.field});
    }
    auto order = [](const Entry &a, const Entry &b) {
        if (a.file != b.file) return a.file < b.file;
        if (a.group != b.group) return a.group < b.group;
        return a.key < b.key;
    };
    std::sort(expected.begin(), expected.end(), order);
    std::sort(requested.begin(), requested.end(), order);
    if (expected.size() != requested.size()) {
        error = QObject::tr("The complete saved Aero palette baseline is required."); return {};
    }
    for (int i = 0; i < requested.size(); ++i) {
        const auto &before = expected.at(i); const auto &after = requested.at(i);
        if (before.file != after.file || before.group != after.group || before.key != after.key) {
            error = QObject::tr("The saved Aero baseline has an invalid palette shape."); return {};
        }
        KConfig config(configName(after.file), isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
        KConfigGroup group(&config, after.group);
        if (!writable(config, group, after.key) || readValue(group, after.key, after.value) != before.value) {
            error = QObject::tr("Aero configuration changed or became locked before saving. Revert to reload it."); return {};
        }
    }
    QList<QPair<Entry,Entry>> result;
    for(int i=0;i<requested.size();++i)if(expected[i].value!=requested[i].value)result.append(qMakePair(expected[i],requested[i]));
    return result;
    }
};
ThemeResult readTheme(const ThemeFiles &files){ThemeResult result;if(files.isolated&&files.options.beforeRead)files.options.beforeRead();result.success=files.read(result);return result;}
ThemeResult saveEntry(const ThemeFiles &files,const QPair<Entry,Entry> &entry,const QByteArray &expectedReceipt,
                      const Reservations::Reservation &reservation,const QStringList &keys,const std::shared_ptr<std::atomic_bool> &changed,
                      const std::shared_ptr<std::atomic_bool> &cancelled)
{
    struct End {Reservations::Reservation lease;~End(){Reservations::endMutation(lease);}}end{reservation};
    auto result=readTheme(files);if(!result.success)return result;result.success=false;
    if(result.receipt!=expectedReceipt||Reservations::currentKeys(reservation.paths,&result.error)!=keys){if(result.error.isEmpty())result.error=QObject::tr("Aero preferences, sources or file identities changed at the save boundary.");return result;}
    const auto &before=entry.first;const auto &after=entry.second;
    {
        KConfig config(files.configName(after.file),files.isolated?KConfig::SimpleConfig:KConfig::FullConfig);KConfigGroup group(&config,after.group);
        if(!writable(config,group,after.key)||readValue(group,after.key,after.value)!=before.value){result.error=QObject::tr("Aero configuration changed or became locked before saving. Some preferences may already be saved; Revert to inspect them.");return result;}
        const auto flags=files.isolated?KConfig::Normal:KConfig::Notify;
        if(after.value.isValid())group.writeEntry(after.key,after.value,flags);else group.deleteEntry(after.key,flags);
        if(files.isolated&&files.options.beforeSync)files.options.beforeSync();
        if(changed->load()||cancelled->load()){config.markAsClean();result.error=QObject::tr("The Aero provider changed or cancellation was requested before native synchronization. No new synchronization was attempted.");return result;}
        QByteArray boundary;QString boundaryError;
        if(!files.receipt(boundary,boundaryError)||boundary!=expectedReceipt||Reservations::currentKeys(reservation.paths)!=keys){config.markAsClean();result.error=boundaryError.isEmpty()?QObject::tr("Aero source bytes or file identities changed immediately before native synchronization."):boundaryError;return result;}
        result.attempted=true;const bool synced=files.isolated&&files.options.syncForTest?files.options.syncForTest(config):config.sync();
        if(!synced){config.markAsClean();result.error=QObject::tr("Aero settings may have been saved partially because native synchronization failed. No rollback was attempted; Revert to inspect saved values.");}
        result.saved=synced;
    }
    result.keys=Reservations::currentKeys(reservation.paths);
    auto saved=readTheme(files);result.state=saved.state;result.receipt=saved.receipt;
    if(files.isolated&&files.options.afterSync)files.options.afterSync();
    if(!result.error.isEmpty())return result;
    if(!saved.success){result.error=saved.error;return result;}
    {
        KConfig verify(files.configName(after.file),files.isolated?KConfig::SimpleConfig:KConfig::FullConfig);const KConfigGroup group(&verify,after.group);
        if(after.value.isValid()?readValue(group,after.key,after.value)!=after.value:group.hasKey(after.key)){result.error=QObject::tr("An Aero value could not be verified after saving. No rollback was attempted; Revert to inspect saved values.");return result;}
    }
    if(changed->load()||cancelled->load()){result.error=QObject::tr("Aero settings were saved, but the provider changed or cancellation was requested during native synchronization. Native broadcast delivery and runtime adoption are uncertain; no further reload was requested.");return result;}
    result.success=true;return result;
}
}
class AeroThemeFileController final : public AeroThemeBackend {
public:
    AeroThemeFileController(const QString &directory,AeroThemeRuntime *runtime,std::unique_ptr<AeroThemeAssetProbe> assets,bool isolated,const AeroThemeBackendOptions &options)
        :AeroThemeBackend(QCoreApplication::instance()),m_files(directory,isolated,options),m_runtime(runtime),m_assets(std::move(assets)){
        runtime->setParent(this);m_deadline.setSingleShot(true);m_remoteDeadline.setSingleShot(true);m_debounce.setSingleShot(true);m_debounce.setInterval(100);
        connect(&m_deadline,&QTimer::timeout,this,[this]{if(m_phase!=Phase::Idle)emit operationPending(tr("Aero settings are taking longer and continue in the background. Wait for completion before retrying or closing."));});
        connect(&m_remoteDeadline,&QTimer::timeout,this,[this]{
            m_timedOut=true;m_runtimeSupported=false;m_runtimeReason=tr("Aero runtime checks timed out; restart the application before applying again.");
            if(m_phase==Phase::Probe){m_runtimeDone=true;finishProbe();}
            else if(m_phase==Phase::Reload){verify(false,tr("Aero runtime reload timed out. Saved settings and sent requests may still have an effect; restart the application before applying again."));}
        });
        connect(runtime,&AeroThemeRuntime::externalChange,this,[this]{m_changed->store(true);m_runtimeSupported=false;emit externalChange();});
        connect(runtime,&AeroThemeRuntime::probeFinished,this,[this](bool supported,const QString &error){if(m_phase!=Phase::Probe||m_runtimeDone)return;m_runtimeSupported=supported;m_runtimeReason=error;m_runtimeDone=true;finishProbe();});
        connect(runtime,&AeroThemeRuntime::reloadFinished,this,[this](bool ok,const QString &error){if(m_phase==Phase::Reload)verify(ok,error);});
        connect(&m_watcher,&QFileSystemWatcher::fileChanged,this,[this]{if(m_phase==Phase::Reload)m_sourcesChanged->store(true);watchFiles();m_debounce.start();});connect(&m_watcher,&QFileSystemWatcher::directoryChanged,this,[this]{if(m_phase==Phase::Reload)m_sourcesChanged->store(true);watchFiles();m_debounce.start();});
        connect(&m_debounce,&QTimer::timeout,this,&AeroThemeBackend::externalChange);watchFiles();
    }
    void orphan(){m_orphaned=true;if(m_phase==Phase::Idle)deleteLater();}
    void cancel()override{if(m_phase==Phase::Idle||!m_write)return;m_cancelled->store(true);emit operationPending(tr("Cancellation was requested. Aero saving continues until native file work finishes; an already running synchronization or sent reload was not cancelled."));}
    void readCurrent()override{if(m_phase!=Phase::Idle)return;m_write=false;start();}
    void writeSettings(const QVariantMap &expected,const QVariantMap &requested)override{
        if(m_phase!=Phase::Idle)return;QString error;
        if(m_timedOut||expected.keys()!=AeroThemeContract::defaults().keys()||!AeroThemeContract::validateRequested(requested,error)){emit writeFinished(false,m_timedOut?tr("Runtime checks timed out; restart the application before applying again."):error.isEmpty()?tr("A complete Aero baseline is required before applying."):error);return;}
        m_write=true;m_expected=expected;m_requested=requested;m_baselineReceipt=m_lastValues==expected?m_lastReceipt:QByteArray();start();
    }
private:
    enum class Phase{Idle,Probe,Read,Admission,Save,Boundary,Reload,Verify};
    template<class Work,class Done>void worker(Work work,Done done){auto *watcher=new QFutureWatcher<ThemeResult>(this);connect(watcher,&QFutureWatcher<ThemeResult>::finished,this,[this,watcher,done=std::move(done)]{auto result=watcher->result();watcher->deleteLater();done(std::move(result));});watcher->setFuture(QtConcurrent::run(std::move(work)));}
    void decorate(AeroThemeState &state){state.assetsSupported=m_assetsSupported;state.runtimeSupported=m_runtimeSupported&&!m_timedOut;state.issues+=m_assetIssues;if(!state.runtimeSupported)state.issues<<(!m_runtimeReason.isEmpty()?m_runtimeReason:tr("The Aero runtime is unavailable."));}
    void start(){
        m_changed=std::make_shared<std::atomic_bool>(false);m_cancelled=std::make_shared<std::atomic_bool>(false);m_sourcesChanged=std::make_shared<std::atomic_bool>(false);m_savedAny=false;m_position=0;m_reservation={};m_phase=Phase::Probe;m_assetsDone=m_runtimeDone=false;
        OperationManager::instance()->setActivity(this,m_write?tr("Saving Aero settings"):tr("Reading Aero settings"),true);
        m_deadline.start(m_files.isolated?qMax(1,m_files.options.softDeadlineMs):20000);
        m_remoteDeadline.start(m_files.isolated?qMax(1,m_files.options.runtimeDeadlineMs):12000);
        const auto assets=m_assets;worker([assets]{ThemeResult result;result.assetsSupported=assets->validate(result.issues);return result;},[this](ThemeResult result){m_assetsSupported=result.assetsSupported;m_assetIssues=result.issues;m_assetsDone=true;finishProbe();});
        if(m_timedOut){m_runtimeDone=true;m_runtimeSupported=false;finishProbe();}else m_runtime->probe();
    }
    void finishProbe(){
        if(m_phase!=Phase::Probe||!m_assetsDone||!m_runtimeDone)return;m_remoteDeadline.stop();m_phase=Phase::Read;const auto files=m_files;
        worker([files]{return readTheme(files);},[this](ThemeResult result){
            decorate(result.state);
            if(!m_write){completeRead(result);return;}
            if(!result.success||result.state.values!=m_expected||(!m_baselineReceipt.isEmpty()&&result.receipt!=m_baselineReceipt)||!result.state.canApply()||m_changed->load()||m_cancelled->load()){
                completeWrite(false,!result.error.isEmpty()?result.error:result.state.values!=m_expected?tr("Aero settings changed before the write. Revert to reload the complete baseline."):tr("Aero assets, runtime, policy or cancellation state changed before saving. %1").arg(result.state.issues.join('\n')));return;
            }
            m_receipt=result.receipt;m_phase=Phase::Admission;const auto inventory=Reservations::snapshot();const auto files=m_files;const auto expected=m_expected,requested=m_requested;
            worker([files,inventory,expected,requested]{auto result=readTheme(files);if(result.success){result.plan=files.prepare(expected,requested,result.error);result.success=result.error.isEmpty();}if(result.success){result.admission=Reservations::inspect(files.targets(),inventory);result.success=result.admission.success;if(!result.success)result.error=result.admission.error;}return result;},[this](ThemeResult result){
                QString error;
                if(!result.success||result.receipt!=m_receipt){completeWrite(false,result.error.isEmpty()?tr("Aero source bytes changed while preparing Apply."):result.error);return;}
                m_reservation=Reservations::acquire(result.admission,error);if(!m_reservation.valid()){completeWrite(false,error);return;}
                m_keys=result.admission.keys;m_plan=std::move(result.plan);saveNext();
            });
        });
    }
    void saveNext(){
        if(m_changed->load()||m_cancelled->load()){completeWrite(false,tr("The Aero provider changed or cancellation was requested. Some values may already be saved; no further reload was requested."));return;}
        if(m_position>=m_plan.size()){boundary();return;}
        if(!Reservations::beginMutation(m_reservation)){completeWrite(false,tr("The Aero file reservation could not enter native synchronization."));return;}
        m_phase=Phase::Save;const auto files=m_files;const auto entry=m_plan[m_position];const auto receipt=m_receipt;const auto lease=m_reservation;const auto keys=m_keys;const auto changed=m_changed,cancelled=m_cancelled;
        worker([files,entry,receipt,lease,keys,changed,cancelled]{return saveEntry(files,entry,receipt,lease,keys,changed,cancelled);},[this](ThemeResult result){
            m_savedAny|=result.saved||result.attempted;
            if(!result.keys.isEmpty()){if(!Reservations::refresh(m_reservation,result.keys)){result.success=false;result.error=tr("Post-save Aero file aliases could not be retained.");}m_keys=result.keys;}
            m_receipt=result.receipt;
            if(!result.success){verify(false,result.error);return;}
            ++m_position;saveNext();
        });
    }
    void boundary(){
        m_phase=Phase::Boundary;const auto files=m_files;const auto paths=m_reservation.paths;const auto keys=m_keys;const auto receipt=m_receipt;const auto assets=m_assets;
        worker([files,paths,keys,receipt,assets]{auto result=readTheme(files);result.assetsSupported=assets->validate(result.issues);result.keys=Reservations::currentKeys(paths,&result.error);if(result.receipt!=receipt||result.keys!=keys){result.success=false;if(result.error.isEmpty())result.error=QObject::tr("Aero source bytes or file identities changed before reload.");}return result;},[this](ThemeResult result){
            if(!result.success||!result.assetsSupported||result.state.values!=m_requested||m_changed->load()||m_cancelled->load()){verify(false,result.error.isEmpty()?tr("Aero settings were saved incompletely or changed before reload; no reload was requested."):result.error);return;}
            m_phase=Phase::Reload;m_remoteDeadline.start(m_files.isolated?qMax(1,m_files.options.runtimeDeadlineMs):12000);
            QSet<QString> changed;for(auto it=m_requested.cbegin();it!=m_requested.cend();++it)if(it.value()!=m_expected.value(it.key()))changed.insert(it.key());
            const auto invalidated=m_changed,cancelled=m_cancelled,sourcesChanged=m_sourcesChanged;
            m_runtime->reloadGuarded(changed,[invalidated,cancelled,sourcesChanged]{return !invalidated->load()&&!cancelled->load()&&!sourcesChanged->load();});
        });
    }
    void verify(bool acknowledged,const QString &error){
        m_remoteDeadline.stop();m_phase=Phase::Verify;const auto files=m_files;const auto paths=m_reservation.paths;const auto assets=m_assets;
        worker([files,paths,assets]{auto result=readTheme(files);result.assetsSupported=assets->validate(result.issues);if(!paths.isEmpty())result.keys=Reservations::currentKeys(paths);return result;},[this,acknowledged,error](ThemeResult result){
            const bool retained=!m_reservation.valid()||(!result.keys.isEmpty()&&Reservations::refresh(m_reservation,result.keys));
            const bool same=retained&&result.assetsSupported&&result.success&&result.state.values==m_requested&&result.receipt==m_receipt;
            const bool ok=acknowledged&&same&&!m_sourcesChanged->load()&&!m_changed->load()&&!m_cancelled->load()&&!m_timedOut;
            completeWrite(ok,ok?QString():!error.isEmpty()?error:!same?tr("Aero settings were saved incompletely or changed during reload. No rollback was attempted; Revert to inspect saved values. %1").arg(result.error):tr("Aero settings were saved, but their runtime changed or reload failed. Native broadcast delivery and runtime adoption remain uncertain; Revert before retrying."));
        });
    }
    void terminal(){m_deadline.stop();m_remoteDeadline.stop();Reservations::release(m_reservation);m_phase=Phase::Idle;watchFiles();OperationManager::instance()->setActivity(this,{},false);}
    void completeRead(const ThemeResult &result){if(result.success){m_lastValues=result.state.values;m_lastReceipt=result.receipt;}terminal();emit readFinished(result.success,result.state,result.error);if(m_orphaned&&m_phase==Phase::Idle)deleteLater();}
    void completeWrite(bool ok,const QString &error){terminal();emit writeFinished(ok,error);if(m_orphaned&&m_phase==Phase::Idle)deleteLater();}
    void watchFiles(){auto paths=m_files.files;if(!m_files.isolated)paths+=AeroThemeContract::installedAssetPaths();for(const auto &path:paths){if(QFileInfo::exists(path)&&!QFileInfo(path).isDir()&&!m_watcher.files().contains(path))m_watcher.addPath(path);auto directory=QFileInfo(path).isDir()?path:QFileInfo(path).absolutePath();while(!QDir(directory).exists()&&directory!=QDir::rootPath())directory=QFileInfo(directory).absolutePath();if(!m_watcher.directories().contains(directory))m_watcher.addPath(directory);}}
    ThemeFiles m_files;AeroThemeRuntime *m_runtime;std::shared_ptr<AeroThemeAssetProbe> m_assets;
    Phase m_phase=Phase::Idle;bool m_write=false,m_orphaned=false,m_timedOut=false,m_assetsDone=false,m_runtimeDone=false,m_assetsSupported=false,m_runtimeSupported=false,m_savedAny=false;
    QString m_runtimeReason;QStringList m_assetIssues,m_keys;QByteArray m_receipt,m_lastReceipt,m_baselineReceipt;QVariantMap m_lastValues;
    QVariantMap m_expected,m_requested;QList<QPair<Entry,Entry>> m_plan;int m_position=0;
    Reservations::Reservation m_reservation;std::shared_ptr<std::atomic_bool> m_changed=std::make_shared<std::atomic_bool>(false),m_cancelled=std::make_shared<std::atomic_bool>(false),m_sourcesChanged=std::make_shared<std::atomic_bool>(false);
    QFileSystemWatcher m_watcher;QTimer m_deadline,m_remoteDeadline,m_debounce;
};
AeroThemeRuntime *createKdeAeroThemeRuntime(const QDBusConnection &bus){return new KdeAeroRuntime(bus);}

KdeAeroThemeBackend::KdeAeroThemeBackend(QObject *parent)
    :KdeAeroThemeBackend(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),createKdeAeroThemeRuntime(QDBusConnection::sessionBus()),std::make_unique<InstalledAssets>(),false,{},parent){}
KdeAeroThemeBackend::KdeAeroThemeBackend(const QString &directory,AeroThemeRuntime *runtime,std::unique_ptr<AeroThemeAssetProbe> assets,QObject *parent)
    :KdeAeroThemeBackend(directory,runtime,std::move(assets),true,{},parent){Q_ASSERT(QFileInfo(directory).isAbsolute());}
KdeAeroThemeBackend::KdeAeroThemeBackend(const QString &directory,AeroThemeRuntime *runtime,std::unique_ptr<AeroThemeAssetProbe> assets,const AeroThemeBackendOptions &options,QObject *parent)
    :KdeAeroThemeBackend(directory,runtime,std::move(assets),true,options,parent){Q_ASSERT(QFileInfo(directory).isAbsolute());}
KdeAeroThemeBackend::KdeAeroThemeBackend(const QString &directory,AeroThemeRuntime *runtime,std::unique_ptr<AeroThemeAssetProbe> assets,bool isolated,const AeroThemeBackendOptions &options,QObject *parent)
    :AeroThemeBackend(parent),m_controller(new AeroThemeFileController(directory,runtime,std::move(assets),isolated,options)){
    connect(m_controller,&AeroThemeBackend::readFinished,this,&AeroThemeBackend::readFinished);connect(m_controller,&AeroThemeBackend::writeFinished,this,&AeroThemeBackend::writeFinished);connect(m_controller,&AeroThemeBackend::externalChange,this,&AeroThemeBackend::externalChange);connect(m_controller,&AeroThemeBackend::operationPending,this,&AeroThemeBackend::operationPending);
}
KdeAeroThemeBackend::~KdeAeroThemeBackend(){m_controller->orphan();}
void KdeAeroThemeBackend::readCurrent(){m_controller->readCurrent();}
void KdeAeroThemeBackend::writeSettings(const QVariantMap &expected,const QVariantMap &requested){m_controller->writeSettings(expected,requested);}
void KdeAeroThemeBackend::cancel(){m_controller->cancel();}
