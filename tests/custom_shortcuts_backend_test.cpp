#include <QtTest>
#include <KConfig>
#include <KConfigGroup>
#include <KDesktopFile>
#include <KService>
#include <KServiceAction>
#include <KShell>
#include <QUrl>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QFutureWatcher>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QtConcurrent>
#include <algorithm>
#include <atomic>
#include <unistd.h>
#include "platform/shortcuts/CustomShortcutsBackend.h"

namespace {
const QString rootXml = QStringLiteral(R"XML(<node><interface name="org.kde.KGlobalAccel">
<method name="allComponents"><arg direction="out" type="ao"/></method>
<method name="getComponent"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
<method name="doRegister"><arg direction="in" type="as"/></method>
<method name="unregister"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="b" direction="out"/></method>
<method name="shortcutKeys"><arg direction="in" type="as"/><arg direction="out" type="a(ai)"/></method>
<method name="defaultShortcutKeys"><arg direction="in" type="as"/><arg direction="out" type="a(ai)"/></method>
</interface></node>)XML");
const QString componentXml = QStringLiteral(R"XML(<node><interface name="org.kde.kglobalaccel.Component">
<property name="uniqueName" type="s" access="read"/>
<method name="getShortcutContexts"><arg direction="out" type="as"/></method>
<method name="allShortcutInfos"><arg direction="in" type="s"/><arg direction="out" type="a(ssssssaiai)"/></method>
<method name="cleanUp"><arg direction="out" type="b"/></method>
</interface></node>)XML");
QString componentPath(const QString &id) { return QStringLiteral("/component/") + QString::fromLatin1(id.toUtf8().toHex()); }
ShortcutAction action(const QString &id, const QString &name = QStringLiteral("_launch"), const QString &context = QStringLiteral("default"))
{
    ShortcutAction result; result.id = {id, context, name}; result.componentFriendly = id; result.contextFriendly = context; result.friendlyName = name; return result;
}
bool put(const QString &path, const QByteArray &contents)
{ QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size(); }
QByteArray get(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
QByteArray desktop(const QString &name, const QString &exec, bool command = false)
{
    return "[Desktop Entry]\nType=Application\nName=" + name.toUtf8() + "\nExec=" + exec.toUtf8() + "\n"
        + (command ? "NoDisplay=true\nStartupNotify=false\nX-KDE-GlobalAccel-CommandShortcut=true\n" : "");
}
CustomShortcutsBackendOptions options(const QTemporaryDir &dir)
{
    CustomShortcutsBackendOptions result; result.isolated = true; result.config = dir.filePath(QStringLiteral("kglobalshortcutsrc"));
    result.dataHome = dir.filePath(QStringLiteral("user")); result.dataDirs = {result.dataHome, dir.filePath(QStringLiteral("system"))};
    result.readBudgetMs = 2000; result.writeBudgetMs = 6000; result.persistenceBudgetMs = 150;
    for (const auto &root : result.dataDirs) QDir().mkpath(root + QStringLiteral("/applications"));
    put(result.config, "[Vendor]\nOpaque=keep\n");
    return result;
}
}
class FakeCustomTransport final : public CustomShortcutsTransport {
public:
    QString owner = QStringLiteral(":1.77"), config;
    QMap<QString, QString> definitions;
    QMap<QString, QStringList> emptyContexts;
    QList<ShortcutAction> actions;
    QStringList methods, destinations;
    QList<QVariantList> arguments;
    int registrations = 0, unregisters = 0, cleanups = 0, delayMs = 0;
    bool badInterface = false, rejectRegister = false, rejectUnregister = false, rejectCleanup = false;
    bool uncertainRegister = false, persistCleanup = true, wrongDefaults = false, onlyWorker = true;
    bool uncertainUnregister = false, retainEmptyComponent = false;
    std::function<void(const QString &)> beforeCall, afterMutation;
    QStringList componentIds() const {
        auto result = emptyContexts.keys();
        for (const auto &entry : actions) if (!result.contains(entry.id.component)) result << entry.id.component;
        return result;
    }
    void prune(const QString &id) {
        if (retainEmptyComponent) { if (!emptyContexts.contains(id)) emptyContexts.insert(id, {}); return; }
        const bool hasDefault = std::any_of(actions.cbegin(), actions.cend(), [&](const auto &entry) { return entry.id.component == id && entry.id.context == QLatin1String("default"); });
        if (hasDefault) return;
        actions.erase(std::remove_if(actions.begin(), actions.end(), [&](const auto &entry) { return entry.id.component == id; }), actions.end());
        emptyContexts.remove(id);
        if (persistCleanup) { KConfig file(config, KConfig::SimpleConfig); file.group("services").group(id).deleteGroup(); file.sync(); }
    }
    CustomShortcutsReply call(const QString &destination, const QString &path, const QString &, const QString &method, const QVariantList &args) override {
        onlyWorker = onlyWorker && QThread::currentThread() != QCoreApplication::instance()->thread();
        methods << method; destinations << destination; arguments << args;
        if (delayMs) QThread::msleep(delayMs);
        if (beforeCall) beforeCall(method);
        CustomShortcutsReply reply{true, false, {}, {}};
        if (method == QLatin1String("GetNameOwner")) { reply.value = owner; return reply; }
        if (destination != owner) return {false, method == QLatin1String("unregister") || method == QLatin1String("cleanUp"), {}, QStringLiteral("Owner vanished")};
        if (method == QLatin1String("Introspect")) reply.value = badInterface ? QStringLiteral("<node/>") : path == QLatin1String("/kglobalaccel") ? rootXml : componentXml;
        else if (method == QLatin1String("allComponents")) {
            QStringList paths; for (const auto &id : componentIds()) paths << componentPath(id); reply.value = paths;
        } else if (method == QLatin1String("Get")) {
            for (const auto &id : componentIds()) if (componentPath(id) == path) { reply.value = id; break; }
            if (reply.value.toString().isEmpty()) return {false, false, {}, QStringLiteral("Unknown component property")};
        } else if (method == QLatin1String("getShortcutContexts")) {
            QStringList contexts{QStringLiteral("default")}; for (const auto &entry : actions) if (componentPath(entry.id.component) == path && !contexts.contains(entry.id.context)) contexts << entry.id.context;
            for (auto it = emptyContexts.cbegin(); it != emptyContexts.cend(); ++it) if (componentPath(it.key()) == path) contexts << it.value();
            reply.value = contexts;
        } else if (method == QLatin1String("allShortcutInfos")) {
            QList<ShortcutAction> found; for (auto entry : actions) if (componentPath(entry.id.component) == path && entry.id.context == args.first().toString()) {
                // Native metadata is lossy; v2 getters must supply full chords.
                for (auto &key : entry.current) key = QKeySequence(key[0].toCombined());
                for (auto &key : entry.defaults) key = QKeySequence(key[0].toCombined()); found << entry;
            } reply.value = QVariant::fromValue(found);
        } else if (method == QLatin1String("shortcutKeys") || method == QLatin1String("defaultShortcutKeys")) {
            const auto id = args.first().toStringList(); const auto parts = id.value(0).split(QLatin1Char('|'));
            const ShortcutActionId identity{parts.value(0), parts.value(1), id.value(1)};
            const auto found = std::find_if(actions.cbegin(), actions.cend(), [&](const auto &entry) { return entry.id == identity; });
            if (found == actions.cend()) return {false, false, {}, QStringLiteral("Unknown action")};
            reply.value = QVariant::fromValue(method == QLatin1String("shortcutKeys") ? found->current : found->defaults);
        } else if (method == QLatin1String("doRegister")) {
            ++registrations;
            if (uncertainRegister) return {false, true, {}, QStringLiteral("Timed out")};
            if (rejectRegister) return {false, false, {}, QStringLiteral("Rejected")};
            const auto native = args.first().toStringList(); const auto parts = native.value(0).split(QLatin1Char('|')); const auto id = parts.value(0);
            if (!componentIds().contains(id)) {
                const auto path = definitions.value(id);
                if (!QFileInfo::exists(path)) return {false, false, {}, QStringLiteral("Missing desktop")};
                KService service(path);
                for (const auto &item : service.actions()) {
                    auto entry = action(id, item.name()); for (const auto &encoded : item.property<QStringList>(QStringLiteral("X-KDE-Shortcuts"))) if (!encoded.isEmpty()) entry.defaults << QKeySequence::fromString(encoded, QKeySequence::PortableText);
                    entry.current = entry.defaults; actions << entry;
                }
                auto entry = action(id); const auto encoded = service.property<QStringList>(QStringLiteral("X-KDE-Shortcuts"));
                for (const auto &key : encoded) if (!key.isEmpty()) entry.defaults << QKeySequence::fromString(key, QKeySequence::PortableText);
                entry.current = entry.defaults; if (wrongDefaults) entry.current = {QKeySequence(QStringLiteral("Meta+F12"))}; actions << entry;
            }
            actions << action(id, native.value(1), parts.value(1, QStringLiteral("default")));
            if (afterMutation) afterMutation(method);
        } else if (method == QLatin1String("unregister")) {
            ++unregisters; if (uncertainUnregister) return {false, true, {}, QStringLiteral("Unregister timed out")};
            const auto parts = args.value(0).toString().split(QLatin1Char('|')); const auto id = parts.value(0), name = args.value(1).toString();
            const bool exists = std::any_of(actions.cbegin(), actions.cend(), [&](const auto &entry) { return entry.id.component == id && entry.id.action == name && entry.id.context == parts.value(1, QStringLiteral("default")); });
            reply.value = exists && !rejectUnregister;
            if (reply.value.toBool()) {
                actions.erase(std::remove_if(actions.begin(), actions.end(), [&](const auto &entry) { return entry.id.component == id && entry.id.action == name; }), actions.end());
                prune(id); if (afterMutation) afterMutation(method);
            }
        }
        else if (method == QLatin1String("cleanUp")) {
            ++cleanups; reply.value = !rejectCleanup;
            if (!rejectCleanup) {
                QString id; for (const auto &entry : actions) if (componentPath(entry.id.component) == path) { id = entry.id.component; break; }
                actions.erase(std::remove_if(actions.begin(), actions.end(), [&](const auto &entry) { return entry.id.component == id; }), actions.end());
                emptyContexts.remove(id);
                if (persistCleanup) { KConfig file(config, KConfig::SimpleConfig); file.group("services").group(id).deleteGroup(); file.sync(); }
                if (afterMutation) afterMutation(method);
            }
        } else return {false, false, {}, QStringLiteral("Unexpected method: ") + method};
        return reply;
    }
};
struct CustomWireInfo { QString name, friendly, component, componentFriendly, context, contextFriendly; QList<int> current, defaults; };
Q_DECLARE_METATYPE(CustomWireInfo)
Q_DECLARE_METATYPE(QList<CustomWireInfo>)
QDBusArgument &operator<<(QDBusArgument &arg, const CustomWireInfo &info)
{ arg.beginStructure(); arg << info.name << info.friendly << info.component << info.componentFriendly << info.context << info.contextFriendly << info.current << info.defaults; arg.endStructure(); return arg; }
const QDBusArgument &operator>>(const QDBusArgument &arg, CustomWireInfo &info)
{ arg.beginStructure(); arg >> info.name >> info.friendly >> info.component >> info.componentFriendly >> info.context >> info.contextFriendly >> info.current >> info.defaults; arg.endStructure(); return arg; }
struct CustomWireKey { QList<int> chords; };
Q_DECLARE_METATYPE(CustomWireKey)
Q_DECLARE_METATYPE(QList<CustomWireKey>)
QDBusArgument &operator<<(QDBusArgument &arg, const CustomWireKey &key)
{ arg.beginStructure(); arg << key.chords; arg.endStructure(); return arg; }
const QDBusArgument &operator>>(const QDBusArgument &arg, CustomWireKey &key)
{ arg.beginStructure(); arg >> key.chords; arg.endStructure(); return arg; }
class CustomWireDaemon final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KGlobalAccel")
public:
    QStringList registration; int writes = 0; bool malformed = false;
public slots:
    QList<QDBusObjectPath> allComponents() { return {QDBusObjectPath(QStringLiteral("/component/wire"))}; }
    QDBusObjectPath getComponent(const QString &) { return QDBusObjectPath(QStringLiteral("/component/wire")); }
    void doRegister(const QStringList &value) { registration = value; ++writes; }
    bool unregister(const QString &, const QString &) { ++writes; return true; }
    QList<CustomWireKey> shortcutKeys(const QStringList &) { return {{malformed ? QList<int>{int(Qt::CTRL | Qt::Key_K)} : QList<int>{int(Qt::CTRL | Qt::Key_K), int(Qt::CTRL | Qt::Key_C), int(Qt::CTRL | Qt::Key_U), int(Qt::CTRL | Qt::Key_X)}}}; }
    QList<CustomWireKey> defaultShortcutKeys(const QStringList &) { return {}; }
};
class CustomWireComponent final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kglobalaccel.Component")
    Q_PROPERTY(QString uniqueName READ uniqueName)
public:
    int cleanups = 0;
    QString uniqueName() const { return QStringLiteral("wire.desktop"); }
public slots:
    QStringList getShortcutContexts() { return {QStringLiteral("default")}; }
    QList<CustomWireInfo> allShortcutInfos(const QString &) { return {{QStringLiteral("_launch"), QStringLiteral("Wire action"), QStringLiteral("wire.desktop"), QStringLiteral("Wire"), QStringLiteral("default"), QStringLiteral("Default"), {int(Qt::CTRL | Qt::Key_K)}, {}}}; }
    bool cleanUp() { ++cleanups; return true; }
};
class CustomWireKWin final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin")
public slots:
    QString supportInformation() { return QStringLiteral("KWin version: 6.6.6\nOperation Mode: Wayland\n"); }
};
class CustomShortcutsBackendTest final : public QObject {
    Q_OBJECT
    CustomShortcutsSnapshot read(KdeCustomShortcutsBackend &backend) {
        QSignalSpy spy(&backend, &CustomShortcutsBackend::readFinished); backend.readCurrent(++token);
        if (spy.isEmpty() && !spy.wait(10000)) qFatal("Custom backend read never completed");
        const auto result = spy.takeFirst(); lastError = result[3].toString(); return qvariant_cast<CustomShortcutsSnapshot>(result[2]);
    }
    QVariantList write(KdeCustomShortcutsBackend &backend, const CustomShortcutsSnapshot &expected, const CustomShortcutChanges &changes) {
        QSignalSpy spy(&backend, &CustomShortcutsBackend::writeFinished); backend.writeChanges(++token, expected, changes);
        if (spy.isEmpty() && !spy.wait(10000)) qFatal("Custom backend write never completed"); return spy.takeFirst();
    }
    std::shared_ptr<FakeCustomTransport> fixture(const CustomShortcutsBackendOptions &o) {
        auto fake = std::make_shared<FakeCustomTransport>(); fake->config = o.config;
        const auto installed = o.dataDirs.last() + QStringLiteral("/applications/vendor.unlisted.desktop");
        put(installed, desktop(QStringLiteral("Vendor application"), QStringLiteral("vendor --open")));
        fake->definitions.insert(QStringLiteral("vendor.unlisted.desktop"), installed);
        const auto command = o.dataHome + QStringLiteral("/applications/net.local.old.desktop");
        put(command, desktop(QStringLiteral("Native KCM command"), QStringLiteral("printf %%s old"), true) + "X-Vendor=keep\n");
        fake->definitions.insert(QStringLiteral("net.local.old.desktop"), command); fake->actions << action(QStringLiteral("net.local.old.desktop"));
        return fake;
    }
private slots:
    void explicitCatalogAndReadbackDoNotRegisterOrBuildCache() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o);
        const auto nested = o.dataDirs.last() + QStringLiteral("/applications/nested"); QVERIFY(QDir().mkpath(nested));
        QVERIFY(put(nested + QStringLiteral("/other.desktop"), desktop(QStringLiteral("Other vendor"), QStringLiteral("other"))));
        QVERIFY(put(o.dataHome + QStringLiteral("/applications/vendor.unlisted.desktop"), "[Desktop Entry]\nHidden=true\n"));
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend);
        QVERIFY2(state.available, qPrintable(lastError)); QVERIFY(state.writable); QVERIFY(!state.application(QStringLiteral("vendor.unlisted.desktop")));
        QVERIFY(state.application(QStringLiteral("nested-other.desktop"))); QVERIFY(state.find(QStringLiteral("net.local.old.desktop"))->editable);
        QCOMPARE(fake->registrations, 0); QCOMPARE(fake->unregisters, 0); QCOMPARE(fake->cleanups, 0); QVERIFY(fake->onlyWorker);
        QVERIFY(!fake->methods.contains(QStringLiteral("invokeShortcut"))); QVERIFY(!fake->methods.contains(QStringLiteral("setForeignShortcutKeys")));
        for (int i = 0; i < fake->methods.size(); ++i) if (fake->methods[i] != QLatin1String("GetNameOwner")) QCOMPARE(fake->destinations[i], fake->owner);
    }
    void unusableOptionalDesktopEntriesDoNotDisableManagerOrLoseReservedIds() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o);
        const auto root = o.dataDirs.last() + QStringLiteral("/applications/"); const auto missing = dir.filePath(QStringLiteral("missing-target"));
        const auto dangling = root + QStringLiteral("broken.optional.desktop");
        QVERIFY(::symlink(QFile::encodeName(missing).constData(), QFile::encodeName(dangling).constData()) == 0);
        const auto unreadable = root + QStringLiteral("unreadable.optional.desktop"); QVERIFY(put(unreadable, desktop(QStringLiteral("Unreadable"), QStringLiteral("echo no"))));
        QVERIFY(QFile::setPermissions(unreadable, QFileDevice::Permissions{}));
        QVERIFY(put(root + QStringLiteral("unsupported.optional.desktop"), "[Desktop Entry]\nType=Link\nURL=https://example.com\n"));
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); QVERIFY2(state.available, qPrintable(lastError)); QVERIFY(state.writable);
        QVERIFY(!state.application(QStringLiteral("broken.optional.desktop"))); QVERIFY(!state.application(QStringLiteral("unreadable.optional.desktop")));
        QVERIFY(!state.application(QStringLiteral("unsupported.optional.desktop"))); QVERIFY(state.application(QStringLiteral("vendor.unlisted.desktop")));
        CustomShortcutChanges changes; changes.addCommands << CustomShortcutCommand{QStringLiteral("broken.optional.desktop"), QStringLiteral("Reserved"), QStringLiteral("echo no")};
        auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Failed); QCOMPARE(fake->registrations, 0);
        changes = {}; changes.addApplications << QStringLiteral("vendor.unlisted.desktop");
        result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
        QVERIFY(QFileInfo(dangling).isSymLink()); QVERIFY(!QFileInfo::exists(missing));
        QVERIFY(QFile::setPermissions(unreadable, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    }
    void optionalLinkTargetAppearanceDuringApplyPreservesCommandAndForeignTarget() {
        QTemporaryDir dir; auto o = options(dir); auto fake = fixture(o);
        const auto target = dir.filePath(QStringLiteral("outside-catalog-target"));
        const auto alias = o.dataDirs.last() + QStringLiteral("/applications/optional.alias.desktop");
        QVERIFY(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(alias).constData()) == 0);
        const auto id = QStringLiteral("net.local.old.desktop"); const auto command = fake->definitions.value(id); const auto original = get(command);
        o.beforeCommit = [&](int, const QString &) { put(target, desktop(QStringLiteral("Appeared"), QStringLiteral("echo foreign"))); };
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); QVERIFY(state.available);
        CustomShortcutChanges changes; changes.editCommands.insert(id, {id, QStringLiteral("Edited"), QStringLiteral("echo ours")});
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Failed);
        QCOMPARE(get(command), original); QVERIFY(get(target).contains("foreign")); QVERIFY(QFileInfo(alias).isSymLink()); QCOMPARE(fake->registrations, 0);
    }
    void applicationRegistrationUsesNativeDummyAndRetainsDesktopOnRemoval() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); KdeCustomShortcutsBackend backend(fake, o);
        const auto original = get(fake->definitions.value(QStringLiteral("vendor.unlisted.desktop"))); const auto state = read(backend);
        CustomShortcutChanges changes; changes.addApplications << QStringLiteral("vendor.unlisted.desktop");
        auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
        QCOMPARE(fake->registrations, 1); QCOMPARE(fake->unregisters, 1);
        const int registration = fake->methods.indexOf(QStringLiteral("doRegister"));
        QCOMPARE(fake->arguments[registration][0].toStringList(), QStringList({QStringLiteral("vendor.unlisted.desktop"), QString(), QStringLiteral("Vendor application"), QString()}));
        QVERIFY(fake->methods.indexOf(QStringLiteral("unregister")) > registration);
        changes = {}; changes.removeComponents << QStringLiteral("vendor.unlisted.desktop");
        result = write(backend, qvariant_cast<CustomShortcutsSnapshot>(result[2]), changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
        QCOMPARE(fake->cleanups, 1); QCOMPARE(get(fake->definitions.value(QStringLiteral("vendor.unlisted.desktop"))), original);
        KConfig config(o.config, KConfig::SimpleConfig); QCOMPARE(config.group("Vendor").readEntry("Opaque"), QStringLiteral("keep"));
    }
    void nativeCommandCreateEditRemovePersistsAndSurvivesBackendRestart() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o);
        const auto id = QStringLiteral("net.local.created.desktop"); const auto path = o.dataHome + QStringLiteral("/applications/") + id; fake->definitions.insert(id, path);
        CustomShortcutsSnapshot state;
        {
            KdeCustomShortcutsBackend backend(fake, o); state = read(backend); CustomShortcutChanges changes;
            changes.addCommands << CustomShortcutCommand{id, QStringLiteral("Created"), QStringLiteral("printf %s value")};
            const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
            state = qvariant_cast<CustomShortcutsSnapshot>(result[2]); QVERIFY(state.find(id)->registered);
        }
        KdeCustomShortcutsBackend restarted(fake, o); state = read(restarted); QVERIFY(state.find(id)); QVERIFY(state.find(id)->editable);
        KDesktopFile saved(path); QVERIFY(saved.desktopGroup().readEntry("X-KDE-GlobalAccel-CommandShortcut", false));
        QCOMPARE(saved.desktopGroup().readEntry("Exec"), QStringLiteral("printf %%s value"));
        CustomShortcutChanges changes; changes.editCommands.insert(id, {id, QStringLiteral("Renamed"), QStringLiteral("echo changed")});
        auto result = write(restarted, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
        QCOMPARE(qvariant_cast<CustomShortcutsSnapshot>(result[2]).find(id)->name, QStringLiteral("Renamed"));
        changes = {}; changes.removeComponents << id; result = write(restarted, qvariant_cast<CustomShortcutsSnapshot>(result[2]), changes);
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success); QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!qvariant_cast<CustomShortcutsSnapshot>(result[2]).find(id));
    }
    void editExistingKcmCommandPreservesUnknownDesktopFieldsAndKeys() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o);
        fake->actions.first().current = {QKeySequence(QStringLiteral("Ctrl+K, Ctrl+C, Ctrl+U, Ctrl+X"))};
        { KConfig config(o.config, KConfig::SimpleConfig); config.group("services").group(QStringLiteral("net.local.old.desktop")).writeEntry("_launch", QStringLiteral("Ctrl+K, Ctrl+C, Ctrl+U, Ctrl+X")); QVERIFY(config.sync()); }
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); QCOMPARE(state.actions.first().current.first().count(), 4);
        CustomShortcutChanges changes; changes.editCommands.insert(QStringLiteral("net.local.old.desktop"), {QStringLiteral("net.local.old.desktop"), QStringLiteral("Edited native"), QStringLiteral("echo %%done")});
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
        KDesktopFile saved(fake->definitions.value(QStringLiteral("net.local.old.desktop"))); QCOMPARE(saved.desktopGroup().readEntry("X-Vendor"), QStringLiteral("keep"));
        QCOMPARE(qvariant_cast<CustomShortcutsSnapshot>(result[2]).actions.first().current, state.actions.first().current); QCOMPARE(fake->registrations, 0);
    }
    void savedUnregisteredKcmCommandCanRegisterAndRemoveWithoutNativeCleanup() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); fake->actions.clear();
        KdeCustomShortcutsBackend backend(fake, o); auto state = read(backend);
        const auto id = QStringLiteral("net.local.old.desktop"); QVERIFY(state.find(id)); QVERIFY(!state.find(id)->registered); QVERIFY(state.find(id)->editable); QVERIFY(state.find(id)->removable);
        CustomShortcutChanges changes; changes.addApplications << id;
        auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success); QCOMPARE(fake->registrations, 1);
        fake->actions.clear(); state = read(backend); changes = {}; changes.removeComponents << id;
        result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
        QCOMPARE(fake->cleanups, 0); QVERIFY(!QFileInfo::exists(fake->definitions.value(id)));
    }
    void savedUnregisteredCommandCanBeEditedAndRegisteredInOneApply() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); fake->actions.clear();
        const auto id = QStringLiteral("net.local.old.desktop"), path = fake->definitions.value(id);
        const auto original = get(path); KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend);
        QVERIFY(state.find(id)); QVERIFY(!state.find(id)->registered); QVERIFY(state.find(id)->editable);
        CustomShortcutChanges changes; changes.addApplications << id;
        changes.editCommands.insert(id, {id, QStringLiteral("Edited and registered"), QStringLiteral("printf %%s combined")});
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
        const auto actual = qvariant_cast<CustomShortcutsSnapshot>(result[2]); const auto *entry = actual.find(id);
        QVERIFY(entry); QVERIFY(entry->registered); QVERIFY(entry->command); QVERIFY(entry->editable);
        QCOMPARE(entry->name, QStringLiteral("Edited and registered")); QCOMPARE(entry->exec, QStringLiteral("printf %%s combined")); QCOMPARE(entry->path, path);
        QVERIFY(entry->identity != state.find(id)->identity); QCOMPARE(actual.contexts.value(id), QStringList{QStringLiteral("default")});
        QCOMPARE(actual.actions.size(), 1); QCOMPARE(actual.actions.first().id, (ShortcutActionId{id, QStringLiteral("default"), QStringLiteral("_launch")}));
        QVERIFY(ShortcutsTypes::sameKeys(actual.actions.first().current, {})); QVERIFY(ShortcutsTypes::sameKeys(actual.actions.first().defaults, {}));
        KDesktopFile saved(path); QCOMPARE(saved.desktopGroup().readEntry("X-Vendor"), QStringLiteral("keep"));
        QCOMPARE(saved.desktopGroup().readEntry("Name"), entry->name); QCOMPARE(saved.desktopGroup().readEntry("Exec"), entry->exec);
        QVERIFY(saved.desktopGroup().readEntry("X-KDE-GlobalAccel-CommandShortcut", false)); QVERIFY(saved.desktopGroup().readEntry("NoDisplay", false)); QVERIFY(get(path) != original);
        QCOMPARE(fake->registrations, 1); QCOMPARE(fake->unregisters, 1); QCOMPARE(fake->cleanups, 0);
        const int registration = fake->methods.indexOf(QStringLiteral("doRegister")), unregister = fake->methods.indexOf(QStringLiteral("unregister"));
        QVERIFY(registration >= 0); QVERIFY(unregister > registration);
        QCOMPARE(fake->arguments[registration][0].toStringList(), QStringList({id, QString(), QStringLiteral("Edited and registered"), QString()}));
        QCOMPARE(fake->arguments[unregister], QVariantList({id, QString()}));
        QVERIFY(!fake->methods.contains(QStringLiteral("invokeShortcut"))); QVERIFY(!fake->methods.contains(QStringLiteral("activateGlobalShortcutContext")));
        QVERIFY(!fake->methods.contains(QStringLiteral("setForeignShortcutKeys")));
        const auto reread = read(backend); QVERIFY(reread.find(id)->registered); QCOMPARE(reread.find(id)->name, entry->name); QCOMPARE(reread.find(id)->exec, entry->exec);
        QCOMPARE(reread.contexts, actual.contexts);
    }
    void applicationActionsAndAllDefaultAlternativesAreVerified() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); const auto id = QStringLiteral("vendor.unlisted.desktop");
        QVERIFY(put(fake->definitions.value(id), desktop(QStringLiteral("Vendor application"), QStringLiteral("vendor --open"))
            + "X-KDE-Shortcuts=Meta+F5,Ctrl+K\nActions=Alternate;\n\n[Desktop Action Alternate]\nName=Alternate action\nExec=vendor --alternate\nX-KDE-Shortcuts=Meta+F6\n"));
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); QVERIFY(state.application(id)); QCOMPARE(state.application(id)->defaults.size(), 2);
        CustomShortcutChanges changes; changes.addApplications << id; const auto result = write(backend, state, changes);
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
        const auto actual = qvariant_cast<CustomShortcutsSnapshot>(result[2]); int count = 0;
        for (const auto &entry : actual.actions) if (entry.id.component == id) { ++count; if (entry.id.action == QLatin1String("_launch")) QCOMPARE(entry.defaults.size(), 2); }
        QCOMPARE(count, 2);
    }
    void fileUrlWithSpacesPersistsAsOneQuotedExecutable() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); const auto id = QStringLiteral("url.desktop");
        const auto executable = dir.filePath(QStringLiteral("a program")); QVERIFY(put(executable, "this fixture is never executed\n"));
        const auto path = o.dataHome + QStringLiteral("/applications/") + id; fake->definitions.insert(id, path);
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); CustomShortcutChanges changes;
        changes.addCommands << CustomShortcutCommand{id, QStringLiteral("Local program"), QUrl::fromLocalFile(executable).toString()};
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
        KDesktopFile saved(path); QCOMPARE(KShell::splitArgs(saved.desktopGroup().readEntry("Exec")), QStringList{executable});
        QCOMPARE(get(executable), QByteArray("this fixture is never executed\n"));
    }
    void immutableLinkedSystemAndShadowedDefinitionsFailClosed() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); const auto oldPath = fake->definitions.value(QStringLiteral("net.local.old.desktop"));
        QVERIFY(put(oldPath, desktop(QStringLiteral("Native"), QStringLiteral("echo old"), true) + "Name[$i]=Protected\n"));
        KdeCustomShortcutsBackend backend(fake, o); auto state = read(backend); QVERIFY(!state.find(QStringLiteral("net.local.old.desktop"))->editable);
        CustomShortcutChanges changes; changes.removeComponents << QStringLiteral("net.local.old.desktop");
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(write(backend, state, changes)[1]), ShortcutsOutcome::Failed); QCOMPARE(fake->cleanups, 0);
        QVERIFY(put(oldPath, desktop(QStringLiteral("Native"), QStringLiteral("echo old"), true)));
        const auto shadow = o.dataDirs.last() + QStringLiteral("/applications/net.local.old.desktop"); QVERIFY(put(shadow, desktop(QStringLiteral("System"), QStringLiteral("echo system"), true)));
        state = read(backend); QVERIFY(state.find(QStringLiteral("net.local.old.desktop"))->editable); QVERIFY(!state.find(QStringLiteral("net.local.old.desktop"))->removable);
        QVERIFY(QFile::remove(shadow)); const auto linked = dir.filePath(QStringLiteral("linked.desktop")); QVERIFY(put(linked, desktop(QStringLiteral("Linked"), QStringLiteral("echo linked"), true)));
        QVERIFY(QFile::remove(oldPath)); QVERIFY(QFile::link(linked, oldPath));
        state = read(backend); QVERIFY(!state.find(QStringLiteral("net.local.old.desktop")) || !state.find(QStringLiteral("net.local.old.desktop"))->editable);
    }
    void immutableRegistryPreventsAllContextRemoval() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o);
        QVERIFY(put(o.config, "[services][net.local.old.desktop]\n_launch[$i]=none\n"));
        fake->actions << action(QStringLiteral("net.local.old.desktop"), QStringLiteral("extra"), QStringLiteral("editor"));
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); QVERIFY(!state.find(QStringLiteral("net.local.old.desktop"))->removable);
        CustomShortcutChanges changes; changes.removeComponents << QStringLiteral("net.local.old.desktop");
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(write(backend, state, changes)[1]), ShortcutsOutcome::Failed); QCOMPARE(fake->cleanups, 0); QCOMPARE(fake->unregisters, 0);
    }
    void allContextRemovalUnregistersDistinctNamesWithoutActivation() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); const auto id = QStringLiteral("net.local.old.desktop");
        fake->actions << action(id, QStringLiteral("extra"), QStringLiteral("editor")) << action(id, QStringLiteral("_launch"), QStringLiteral("editor"));
        fake->emptyContexts.insert(id, {QStringLiteral("empty-editor")});
        { KConfig file(o.config, KConfig::SimpleConfig); file.group("services").group(id).group("empty-editor").writeEntry("Opaque", "old-target-state"); QVERIFY(file.sync()); }
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); QVERIFY(state.find(id)->removable); QCOMPARE(state.contexts.value(id).size(), 3);
        CustomShortcutChanges changes; changes.removeComponents << id; const auto result = write(backend, state, changes);
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success); QCOMPARE(fake->cleanups, 0); QCOMPARE(fake->unregisters, 2); QCOMPARE(fake->registrations, 0);
        QVERIFY(!qvariant_cast<CustomShortcutsSnapshot>(result[2]).contexts.contains(id)); QVERIFY(!QFileInfo::exists(fake->definitions.value(id))); QVERIFY(fake->actions.isEmpty());
        const int first = fake->methods.indexOf(QStringLiteral("unregister"));
        QCOMPARE(fake->arguments[first], QVariantList({id + QStringLiteral("|editor"), QStringLiteral("extra")}));
        QVERIFY(!fake->methods.contains(QStringLiteral("activateGlobalShortcutContext")));
        KConfig saved(o.config, KConfig::SimpleConfig); QVERIFY(!saved.group("services").hasGroup(id)); QCOMPARE(saved.group("Vendor").readEntry("Opaque"), QStringLiteral("keep"));
    }
    void nondefaultOnlyAndFullyEmptyComponentsAreCompletelyPruned_data() {
        QTest::addColumn<bool>("actionless"); QTest::newRow("nondefault-only") << false; QTest::newRow("actionless-with-empty-context") << true;
    }
    void nondefaultOnlyAndFullyEmptyComponentsAreCompletelyPruned() {
        QFETCH(bool, actionless); QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); const auto id = QStringLiteral("net.local.old.desktop");
        fake->actions.clear(); if (!actionless) fake->actions << action(id, QStringLiteral("only-editor"), QStringLiteral("editor"));
        fake->emptyContexts.insert(id, {QStringLiteral("empty-editor")});
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); QVERIFY(state.find(id)->registered); QVERIFY(state.find(id)->removable);
        CustomShortcutChanges changes; changes.removeComponents << id; const auto result = write(backend, state, changes);
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success); QCOMPARE(fake->cleanups, 0); QCOMPARE(fake->registrations, actionless ? 1 : 0); QCOMPARE(fake->unregisters, 1);
        QVERIFY(!qvariant_cast<CustomShortcutsSnapshot>(result[2]).contexts.contains(id)); QVERIFY(fake->componentIds().isEmpty());
        QVERIFY(!fake->methods.contains(QStringLiteral("activateGlobalShortcutContext"))); QVERIFY(!fake->methods.contains(QStringLiteral("invokeShortcut")));
    }
    void emptyContextPolicyIsEnforcedWithoutAnyRegisteredActions() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); const auto id = QStringLiteral("net.local.old.desktop"); fake->actions.clear(); fake->emptyContexts.insert(id, {QStringLiteral("protected-empty")});
        QVERIFY(put(o.config, "[services][net.local.old.desktop][protected-empty][$i]\nOpaque=protected\n"));
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); QVERIFY(state.find(id)); QVERIFY(!state.find(id)->removable);
        CustomShortcutChanges changes; changes.removeComponents << id; QCOMPARE(qvariant_cast<ShortcutsOutcome>(write(backend, state, changes)[1]), ShortcutsOutcome::Failed);
        QCOMPARE(fake->registrations, 0); QCOMPARE(fake->unregisters, 0); QVERIFY(QFileInfo::exists(fake->definitions.value(id)));
    }
    void delayedDefaultEmptyPruningIsReconciledBetweenUnregisterCalls() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); const auto id = QStringLiteral("net.local.old.desktop");
        fake->actions = {action(id, QStringLiteral("first"), QStringLiteral("editor")), action(id, QStringLiteral("second"), QStringLiteral("editor"))};
        fake->retainEmptyComponent = true; int ownerReads = 0;
        fake->beforeCall = [&](const QString &method) {
            if (fake->unregisters == 1 && method == QLatin1String("GetNameOwner") && ++ownerReads == 3) { fake->retainEmptyComponent = false; fake->prune(id); }
        };
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); CustomShortcutChanges changes; changes.removeComponents << id;
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Success);
        QCOMPARE(fake->unregisters, 1); QVERIFY(fake->componentIds().isEmpty()); QVERIFY(!QFileInfo::exists(fake->definitions.value(id)));
    }
    void partialContextRemovalPreservesForeignChangesAndStops() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); const auto id = QStringLiteral("net.local.old.desktop"); fake->actions << action(id, QStringLiteral("extra"), QStringLiteral("editor"));
        fake->afterMutation = [&](const QString &method) { if (method == QLatin1String("unregister")) { KConfig file(o.config, KConfig::SimpleConfig); file.group("Vendor").writeEntry("Foreign", "preserve"); file.sync(); } };
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); CustomShortcutChanges changes; changes.removeComponents << id;
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Partial); QCOMPARE(fake->unregisters, 1); QCOMPARE(fake->actions.size(), 1);
        KConfig saved(o.config, KConfig::SimpleConfig); QCOMPARE(saved.group("Vendor").readEntry("Foreign"), QStringLiteral("preserve"));
    }
    void uncertainContextUnregisterLocksOwnerAndPruningTimeoutIsPartial_data() {
        QTest::addColumn<bool>("uncertain"); QTest::newRow("uncertain-unregister") << true; QTest::newRow("prune-timeout") << false;
    }
    void uncertainContextUnregisterLocksOwnerAndPruningTimeoutIsPartial() {
        QFETCH(bool, uncertain); QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); const auto id = QStringLiteral("net.local.old.desktop"); fake->emptyContexts.insert(id, {QStringLiteral("empty-editor")});
        fake->uncertainUnregister = uncertain; fake->retainEmptyComponent = !uncertain;
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); CustomShortcutChanges changes; changes.removeComponents << id;
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), uncertain ? ShortcutsOutcome::Uncertain : ShortcutsOutcome::Partial);
        QCOMPARE(read(backend).writable, !uncertain); QVERIFY(!fake->methods.contains(QStringLiteral("activateGlobalShortcutContext")));
    }
    void hiddenDesktopIdentityCannotBeReusedForCommand() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o);
        QVERIFY(put(o.dataDirs.last() + QStringLiteral("/applications/reserved.desktop"), "[Desktop Entry]\nHidden=true\n"));
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); CustomShortcutChanges changes;
        changes.addCommands << CustomShortcutCommand{QStringLiteral("reserved.desktop"), QStringLiteral("Reserved"), QStringLiteral("echo no")};
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Failed);
        QVERIFY(!QFileInfo::exists(o.dataHome + QStringLiteral("/applications/reserved.desktop"))); QCOMPARE(fake->registrations, 0);
    }
    void emptyContextAppearingBeforeRemovalPreservesFileAndRegistry() {
        QTemporaryDir dir; auto o = options(dir); auto fake = fixture(o);
        const auto id = QStringLiteral("net.local.old.desktop"); const auto path = fake->definitions.value(id); const auto original = get(path);
        o.beforeCommit = [&](int, const QString &) { fake->emptyContexts.insert(id, {QStringLiteral("new-empty-context")}); };
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); QVERIFY(state.find(id)->removable);
        CustomShortcutChanges changes; changes.removeComponents << id;
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Failed);
        QCOMPARE(get(path), original); QCOMPARE(fake->cleanups, 0); QCOMPARE(fake->registrations, 0); QCOMPARE(fake->actions.size(), 1);
        QVERIFY(result[3].toString().contains(QStringLiteral("registry")));
    }
    void savedOverrideForUnregisteredApplicationRefusesBeforeAnyMutation() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o);
        { KConfig config(o.config, KConfig::SimpleConfig); config.group("services").group(QStringLiteral("vendor.unlisted.desktop")).writeEntry("_launch", QStringLiteral("Meta+Q")); QVERIFY(config.sync()); }
        const auto baseline = get(o.config); KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend);
        CustomShortcutChanges changes; changes.addApplications << QStringLiteral("vendor.unlisted.desktop");
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Failed);
        QCOMPARE(fake->registrations, 0); QCOMPARE(fake->unregisters, 0); QCOMPARE(get(o.config), baseline); QVERIFY(result[3].toString().contains(QStringLiteral("overrides")));
    }
    void occupiedApplicationDefaultRefusesBeforeRegistrationWithoutStealing() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o);
        fake->actions.first().current = {QKeySequence(QStringLiteral("Meta+Q"))};
        { KConfig config(o.config, KConfig::SimpleConfig); config.group("services").group(QStringLiteral("net.local.old.desktop")).writeEntry("_launch", QStringLiteral("Meta+Q")); QVERIFY(config.sync()); }
        QVERIFY(put(fake->definitions.value(QStringLiteral("vendor.unlisted.desktop")), desktop(QStringLiteral("Vendor application"), QStringLiteral("vendor --open")) + "X-KDE-Shortcuts=Meta+Q\n"));
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); CustomShortcutChanges changes; changes.addApplications << QStringLiteral("vendor.unlisted.desktop");
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Failed); QCOMPARE(fake->registrations, 0);
        QCOMPARE(fake->actions.first().current, state.actions.first().current); QVERIFY(result[3].toString().contains(QStringLiteral("occupied")));
    }
    void staleDiskCatalogAndOwnerRefuseMutation() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); KdeCustomShortcutsBackend backend(fake, o);
        const auto state = read(backend); QVERIFY(put(o.config, "[Vendor]\nOpaque=external\n")); CustomShortcutChanges changes;
        changes.editCommands.insert(QStringLiteral("net.local.old.desktop"), {QStringLiteral("net.local.old.desktop"), QStringLiteral("New"), QStringLiteral("echo new")});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(write(backend, state, changes)[1]), ShortcutsOutcome::Failed); QVERIFY(get(o.config).contains("external"));
        const auto fresh = read(backend); fake->owner = QStringLiteral(":1.88");
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(write(backend, fresh, changes)[1]), ShortcutsOutcome::Failed); QCOMPARE(fake->registrations, 0);
    }
    void definitionRaceRollsBackOnlyOurEarlierCommit() {
        QTemporaryDir dir; auto o = options(dir); auto fake = fixture(o); const auto first = fake->definitions.value(QStringLiteral("net.local.old.desktop")); const auto original = get(first);
        const auto next = QStringLiteral("next.desktop"); const auto nextPath = o.dataHome + QStringLiteral("/applications/") + next; fake->definitions.insert(next, nextPath);
        const auto external = o.dataDirs.last() + QStringLiteral("/applications/external.desktop");
        o.beforeCommit = [&](int index, const QString &) { if (index == 1) put(external, desktop(QStringLiteral("External"), QStringLiteral("echo foreign"))); };
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); CustomShortcutChanges changes;
        changes.addCommands << CustomShortcutCommand{next, QStringLiteral("Next"), QStringLiteral("echo next")};
        changes.editCommands.insert(QStringLiteral("net.local.old.desktop"), {QStringLiteral("net.local.old.desktop"), QStringLiteral("New"), QStringLiteral("echo new")});
        // Adds are prepared first, so the first commit is rolled back when the second preflight sees a foreign catalog file.
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Failed);
        QCOMPARE(get(first), original); QVERIFY(!QFileInfo::exists(nextPath)); QVERIFY(get(external).contains("foreign")); QCOMPARE(fake->registrations, 0);
    }
    void foreignEditAfterCommitIsPreservedAndReportedPartial() {
        QTemporaryDir dir; auto o = options(dir); auto fake = fixture(o); const auto path = fake->definitions.value(QStringLiteral("net.local.old.desktop"));
        o.afterCommit = [&](int, const QString &changed) { put(changed, desktop(QStringLiteral("Foreign"), QStringLiteral("echo foreign"), true)); };
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); CustomShortcutChanges changes;
        changes.editCommands.insert(QStringLiteral("net.local.old.desktop"), {QStringLiteral("net.local.old.desktop"), QStringLiteral("Ours"), QStringLiteral("echo ours")});
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Partial);
        QVERIFY(get(path).contains("Foreign")); QCOMPARE(fake->registrations, 0);
    }
    void symlinkSwapDuringApplyCannotRedirectWrite() {
        QTemporaryDir dir; auto o = options(dir); auto fake = fixture(o); const auto path = fake->definitions.value(QStringLiteral("net.local.old.desktop"));
        const auto victim = dir.filePath(QStringLiteral("victim")); QVERIFY(put(victim, "untouched\n"));
        o.beforeCommit = [&](int, const QString &changed) { QFile::remove(changed); QFile::link(victim, changed); };
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); CustomShortcutChanges changes;
        changes.editCommands.insert(QStringLiteral("net.local.old.desktop"), {QStringLiteral("net.local.old.desktop"), QStringLiteral("Ours"), QStringLiteral("echo ours")});
        QCOMPARE(qvariant_cast<ShortcutsOutcome>(write(backend, state, changes)[1]), ShortcutsOutcome::Failed); QCOMPARE(get(victim), QByteArray("untouched\n")); QVERIFY(QFileInfo(path).isSymLink());
    }
    void nativeRejectedBooleanIsNotSuccess() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); KdeCustomShortcutsBackend backend(fake, o); auto state = read(backend);
        fake->rejectUnregister = true; CustomShortcutChanges changes; changes.addApplications << QStringLiteral("vendor.unlisted.desktop");
        auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Partial);
        fake->rejectUnregister = false; fake->rejectCleanup = true; changes = {}; changes.removeComponents << QStringLiteral("vendor.unlisted.desktop");
        state = read(backend); result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Partial);
        QVERIFY(qvariant_cast<CustomShortcutsSnapshot>(result[2]).find(QStringLiteral("vendor.unlisted.desktop")));
    }
    void uncertainRegistrationLocksOwnerUntilRestart() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend);
        fake->uncertainRegister = true; CustomShortcutChanges changes; changes.addApplications << QStringLiteral("vendor.unlisted.desktop");
        auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Uncertain);
        QVERIFY(!read(backend).writable); fake->uncertainRegister = false;
        result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Failed); QCOMPARE(fake->registrations, 1);
        fake->owner = QStringLiteral(":1.99"); QVERIFY(read(backend).writable);
    }
    void cleanupPersistenceTimeoutIsPartialAndKnownStateCanReload() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); fake->persistCleanup = false;
        fake->actions.first().current = {QKeySequence(QStringLiteral("Meta+Q"))};
        { KConfig config(o.config, KConfig::SimpleConfig); config.group("services").group(QStringLiteral("net.local.old.desktop")).writeEntry("_launch", QStringLiteral("Meta+Q")); QVERIFY(config.sync()); }
        KdeCustomShortcutsBackend backend(fake, o); const auto state = read(backend); CustomShortcutChanges changes; changes.removeComponents << QStringLiteral("net.local.old.desktop");
        const auto result = write(backend, state, changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Partial);
        QVERIFY(result[3].toString().contains(QStringLiteral("persistence"))); QVERIFY(read(backend).writable); QVERIFY(!QFileInfo::exists(fake->definitions.value(QStringLiteral("net.local.old.desktop"))));
    }
    void registrationReadbackChecksCompleteDefaultsAndStopsAfterForeignEdit() {
        QTemporaryDir dir; const auto o = options(dir); auto fake = fixture(o); fake->wrongDefaults = true;
        KdeCustomShortcutsBackend backend(fake, o); CustomShortcutChanges changes; changes.addApplications << QStringLiteral("vendor.unlisted.desktop");
        const auto result = write(backend, read(backend), changes); QCOMPARE(qvariant_cast<ShortcutsOutcome>(result[1]), ShortcutsOutcome::Partial);
        QVERIFY(!result[3].toString().isEmpty());
    }
    void unsupportedInterfaceAndReadBudgetFailClosedOffGuiThread() {
        QTemporaryDir dir; auto o = options(dir); o.readBudgetMs = 15; auto fake = fixture(o); fake->badInterface = true;
        KdeCustomShortcutsBackend backend(fake, o); QVERIFY(!read(backend).available); fake->badInterface = false; fake->delayMs = 10;
        int ticks = 0; QTimer timer; timer.setInterval(1); connect(&timer, &QTimer::timeout, this, [&] { ++ticks; }); timer.start();
        QElapsedTimer elapsed; elapsed.start(); QVERIFY(!read(backend).available); QVERIFY(elapsed.elapsed() < 1000); QVERIFY(ticks > 2); QVERIFY(fake->onlyWorker); QCOMPARE(fake->registrations, 0);
    }
    void destroyingBackendDuringWorkerReadIsSafe() {
        QTemporaryDir dir; auto o = options(dir); o.beforeScan = [] { QThread::msleep(50); }; auto fake = fixture(o);
        auto *backend = new KdeCustomShortcutsBackend(fake, o); backend->readCurrent(1); delete backend;
        QTest::qWait(250); QCOMPARE(fake->registrations, 0);
    }
    void privateBusWireContractsAndMalformedChords() {
        qDBusRegisterMetaType<CustomWireInfo>(); qDBusRegisterMetaType<QList<CustomWireInfo>>();
        qDBusRegisterMetaType<CustomWireKey>(); qDBusRegisterMetaType<QList<CustomWireKey>>(); qDBusRegisterMetaType<QList<QDBusObjectPath>>();
        QProcess daemon; daemon.start(QStringLiteral("dbus-daemon"), {QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1")});
        QVERIFY(daemon.waitForReadyRead(3000)); const auto address = QString::fromUtf8(daemon.readLine()).trimmed(); QVERIFY(!address.isEmpty());
        const auto name = QStringLiteral("custom-wire-") + QString::number(::getpid()); auto bus = QDBusConnection::connectToBus(address, name); QVERIFY(bus.isConnected());
        CustomWireDaemon root; CustomWireComponent component; CustomWireKWin kwin;
        QVERIFY(bus.registerService(QStringLiteral("org.kde.kglobalaccel"))); QVERIFY(bus.registerObject(QStringLiteral("/kglobalaccel"), &root, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerObject(QStringLiteral("/component/wire"), &component, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
        QVERIFY(bus.registerService(QStringLiteral("org.kde.KWin"))); QVERIFY(bus.registerObject(QStringLiteral("/KWin"), &kwin, QDBusConnection::ExportAllSlots));
        QTemporaryDir dir; const auto o = options(dir); auto transport = createCustomShortcutsDBusTransport(bus); KdeCustomShortcutsBackend backend(transport, o);
        const auto state = read(backend); QVERIFY2(state.available, qPrintable(lastError)); QCOMPARE(state.actions.size(), 1); QCOMPARE(state.actions.first().current.first().count(), 4);
        QCOMPARE(root.writes, 0); QCOMPARE(component.cleanups, 0);
        auto call = [&](const QString &path, const QString &interface, const QString &method, const QVariantList &args) {
            QFutureWatcher<CustomShortcutsReply> future; QSignalSpy finished(&future, &QFutureWatcher<CustomShortcutsReply>::finished);
            future.setFuture(QtConcurrent::run([=] { return transport->call(bus.baseService(), path, interface, method, args); }));
            if (finished.isEmpty() && !finished.wait(5000)) qFatal("Private bus reply did not complete"); return future.result();
        };
        const auto support = call(QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("supportInformation"), {});
        QVERIFY(support.success); QCOMPARE(support.value.toString(), QStringLiteral("KWin version: 6.6.6\nOperation Mode: Wayland\n"));
        QCOMPARE(bus.interface()->serviceOwner(QStringLiteral("org.kde.KWin")).value(), bus.interface()->serviceOwner(QStringLiteral("org.kde.kglobalaccel")).value());
        QCOMPARE(bus.interface()->servicePid(QStringLiteral("org.kde.KWin")).value(), uint(::getpid()));
        QVERIFY(call(QStringLiteral("/kglobalaccel"), QStringLiteral("org.kde.KGlobalAccel"), QStringLiteral("doRegister"), {QStringList{QStringLiteral("wire.desktop"), QString(), QStringLiteral("Wire"), QString()}}).success);
        QCOMPARE(root.registration.size(), 4); QVERIFY(call(QStringLiteral("/kglobalaccel"), QStringLiteral("org.kde.KGlobalAccel"), QStringLiteral("unregister"), {QStringLiteral("wire.desktop"), QString()}).value.toBool());
        QVERIFY(call(QStringLiteral("/component/wire"), QStringLiteral("org.kde.kglobalaccel.Component"), QStringLiteral("cleanUp"), {}).value.toBool());
        root.malformed = true; QVERIFY(!call(QStringLiteral("/kglobalaccel"), QStringLiteral("org.kde.KGlobalAccel"), QStringLiteral("shortcutKeys"), {QStringList{QStringLiteral("wire.desktop|default"), QStringLiteral("_launch"), QString(), QString()}}).success);
        bus.unregisterService(QStringLiteral("org.kde.KWin")); bus.unregisterService(QStringLiteral("org.kde.kglobalaccel")); QDBusConnection::disconnectFromBus(name); daemon.terminate(); QVERIFY(daemon.waitForFinished(3000));
    }
private:
    quint64 token = 0; QString lastError;
};
QTEST_GUILESS_MAIN(CustomShortcutsBackendTest)
#include "custom_shortcuts_backend_test.moc"
