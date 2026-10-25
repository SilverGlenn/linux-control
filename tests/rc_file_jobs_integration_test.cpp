#include "operations/OperationManager.h"
#include "platform/kde/AeroThemeBackend.h"
#include "platform/kde/AppearanceBackend.h"
#include "platform/kde/FontBackend.h"
#include "platform/kde/NightLightBackend.h"
#include "platform/keyboard/KeyboardBackend.h"
#include "platform/apps/DefaultAppsBackend.h"
#include "platform/shortcuts/StandardShortcutsBackend.h"
#include "settings/AeroThemeContract.h"
#include "settings/ShortcutsSettings.h"
#include <KConfig>
#include <KConfigGroup>
#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace {
enum Kind { Aero, Filter, Appearance, Font, Apps, Standards, Keyboard };
struct Gate {
    std::atomic_bool armed{true},entered{false};
    QSemaphore released;
    void once() { if (armed.exchange(false)) { entered = true; released.acquire(); } }
};
class Assets final : public AeroThemeAssetProbe {
public:
    bool validate(QStringList &issues) const override { issues.clear(); return true; }
};
class ThemeRuntime final : public AeroThemeRuntime {
public:
    void probe() override { emit probeFinished(true, {}); }
    void reload(const QSet<QString> &) override { emit reloadFinished(true, {}); }
};
class WindowRuntime final : public AppearanceReloadService {
public:
    bool isAvailable() const override { return true; }
    QString unavailableReason() const override { return {}; }
    QString identity() const override { return QStringLiteral(":fixture.window.v1"); }
    void requestReload(const QString &) override { emit finished(true, {}); }
};
class FontRuntime final : public FontReloadService {
public:
    bool isAvailable() const override { return true; }
    QString unavailableReason() const override { return {}; }
    QString identity() const override { return QStringLiteral(":fixture.font.v1"); }
    void requestReload() override { emit finished(true, {}); }
};
class FilterRuntime final : public NightLightService {
public:
    void query(quint64 token, const QString & = {}) override {
        NightLightRuntime runtime;
        runtime.owner = QStringLiteral(":fixture.filter.v1"); runtime.epoch = 1;
        runtime.version = QStringLiteral("6.6.6");
        runtime.serviceAvailable = runtime.schemaSupported = runtime.known = runtime.available = runtime.daylight = true;
        runtime.mode = 1; runtime.currentTemperature = runtime.targetTemperature = 6500;
        emit queryFinished(token, true, runtime, {});
    }
    void preview(quint64, int, const QString &, quint64) override { QFAIL("Cross-provider fixtures must not preview"); }
    void stopPreview(quint64, const QString &, quint64) override { QFAIL("Cross-provider fixtures must not stop previews"); }
};
class KeyboardRuntime final : public KeyboardTransport {
public:
    void call(const QString &, const QString &, const QString &, const QString &method,
              const QVariantList &, Completion completion) override {
        KeyboardReply reply; reply.success = true;
        if (method == QLatin1String("GetNameOwner")) reply.value = QStringLiteral(":fixture.keyboard.v1");
        else if (method == QLatin1String("supportInformation")) reply.value = QStringLiteral("KWin version: 6.6.6\n");
        else if (method == QLatin1String("Introspect")) reply.value = QStringLiteral("<node><interface name=\"org.kde.KeyboardLayouts\"><method name=\"getLayoutsList\"><arg type=\"a(sss)\" direction=\"out\"/></method></interface></node>");
        else if (method == QLatin1String("getLayoutsList")) reply.value = QVariantList{QVariantMap{{"shortName", "us"}, {"displayName", "US"}, {"longName", "English (US)"}}};
        else reply.success = false;
        QTimer::singleShot(0, this, [completion = std::move(completion), reply] { completion(reply); });
    }
    bool notify(const QString &owner, const QString &, const QString &, const QStringList &) override {
        return owner == QLatin1String(":fixture.keyboard.v1");
    }
};
struct Endpoint {
    QPointer<SettingsSession> session;
    QPointer<QObject> service;
    bool hasService = false;
    std::unique_ptr<QSignalSpy> completed;
    std::function<bool()> ready;
    std::function<void()> stage;
};
}

class RcFileJobsIntegrationTest final : public QObject {
    Q_OBJECT
    std::vector<std::unique_ptr<Endpoint>> endpoints;
    QList<std::shared_ptr<Gate>> gates;
    QString home, data;
    QString config(const QString &name) const { return home + '/' + name; }
    void save(const QString &path, const QByteArray &bytes) {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QSaveFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size()); QVERIFY(file.commit());
    }
    QByteArray bytes(const QString &path) const {
        QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    void seed() {
        const auto defaults = AeroThemeContract::defaults();
        for (const auto &field : AeroThemeContract::fields()) {
            KConfig file(config(field.file), KConfig::SimpleConfig);
            if (field.key == QLatin1String("palette")) {
                const auto palette = defaults.value(field.key).toMap();
                for (auto group = palette.cbegin(); group != palette.cend(); ++group) {
                    const auto entries = group.value().toMap(); KConfigGroup target(&file, group.key());
                    for (auto entry = entries.cbegin(); entry != entries.cend(); ++entry) target.writeEntry(entry.key(), entry.value());
                }
            } else KConfigGroup(&file, field.group).writeEntry(field.entry, defaults.value(field.key));
            KConfigGroup(&file, "Unrelated").writeEntry("Keep", "unowned"); QVERIFY(file.sync());
        }
        {
            KConfig file(config("kwinrc"), KConfig::SimpleConfig);
            auto night = file.group("NightColor"); night.writeEntry("Active", false); night.writeEntry("Mode", 1);
            night.writeEntry("NightTemperature", 6500); night.writeEntry("DayTemperature", 6500);
            file.group("Windows").writeEntry("TitlebarDoubleClickCommand", "Maximize"); QVERIFY(file.sync());
        }
        {
            KConfig file(config("kdeglobals"), KConfig::SimpleConfig);
            file.group("General").writeEntry("TerminalService", "old-terminal.desktop");
            file.group("General").writeEntry("TerminalApplication", "fixture-old-terminal");
            file.group("BatteryManagement").writeEntry("BatteryChargeStopThreshold", 80); QVERIFY(file.sync());
        }
        save(config("kxkbrc"), "[Unrelated]\nKeep=unowned\n");
        save(config("fontconfig/fonts.conf"), "<?xml version=\"1.0\"?><fontconfig><!-- Preserve this unrelated comment --></fontconfig>\n");
        save(config("evdev.xml"), "<xkbConfigRegistry><modelList><model><configItem><name>pc104</name><description>Generic 104</description></configItem></model></modelList><layoutList><layout><configItem><name>us</name><description>English (US)</description></configItem></layout></layoutList></xkbConfigRegistry>");
        save(data + "/applications/old-terminal.desktop", "[Desktop Entry]\nType=Application\nName=Fixture Old Terminal\nExec=fixture-old-terminal\nCategories=TerminalEmulator;\nTerminal=true\n");
        save(data + "/applications/new-terminal.desktop", "[Desktop Entry]\nType=Application\nName=Fixture New Terminal\nExec=fixture-new-terminal\nCategories=TerminalEmulator;\nTerminal=true\n");
    }
    Endpoint *make(Kind kind, bool fullGlobals, bool workspace, const std::shared_ptr<Gate> &gate = {}) {
        auto value = std::make_unique<Endpoint>(); auto *endpoint = value.get();
        const std::function<void()> hold = gate ? std::function<void()>([gate] { gate->once(); }) : std::function<void()>();
        if (kind == Aero) {
            auto *runtime = new ThemeRuntime; AeroThemeBackendOptions options; options.beforeSync = hold; options.softDeadlineMs = 20;
            auto *session = new AeroThemeSettings(new KdeAeroThemeBackend(home, runtime, std::make_unique<Assets>(), options));
            endpoint->session = session; endpoint->service = runtime;
            endpoint->ready = [session] { return session->isReady() && session->canApply(); };
            endpoint->stage = [session] { session->selectValue("blurStrength", 7); };
        } else if (kind == Filter) {
            auto *runtime = new FilterRuntime; KdeNightLightBackend::ConfigContext context;
            context.fixture = true; context.fullCascade = fullGlobals; context.softDeadlineMs = 20; context.beforeSync = hold;
            auto *session = new NightLightSettings(new KdeNightLightBackend(config("kwinrc"), runtime, context));
            endpoint->session = session; endpoint->service = runtime;
            endpoint->ready = [session] { return session->isReady() && session->canApply(); };
            endpoint->stage = [session] { session->selectValue(NightLightKey::NightTemperature, 4200); };
        } else if (kind == Appearance) {
            auto *runtime = new WindowRuntime; KdeAppearanceBackend::FileContext context;
            context.fixture = true; context.beforeSync = hold; context.softDeadlineMs = 20;
            auto *session = new AppearanceSettings(new KdeAppearanceBackend(config("kwinrc"), runtime, context));
            endpoint->session = session; endpoint->service = runtime;
            endpoint->ready = [session] { return session->isReady() && session->canApply(); };
            endpoint->stage = [session] { session->selectAction("Nothing"); };
        } else if (kind == Font) {
            auto *runtime = new FontRuntime; KdeFontBackend::FileContext context;
            context.fixture = true; context.fullCascade = true; context.beforeSync = hold; context.softDeadlineMs = 20;
            auto *session = new FontSettings(new KdeFontBackend(config("kdeglobals"), config("fontconfig/fonts.conf"), runtime, context));
            endpoint->session = session; endpoint->service = runtime;
            endpoint->ready = [session] { return session->isReady() && session->canApply(); };
            endpoint->stage = [session] { auto font = session->stagedPreferences().fonts[0]; font.setPointSize(11); session->selectFont(0, font); };
        } else if (kind == Apps) {
            DefaultAppsPaths paths; paths.configHome = home; paths.dataDirs = {data}; paths.isolated = true; paths.softDeadlineMs = 20;
            if (gate) paths.beforeCommit = [gate](int, const QString &) { gate->once(); };
            auto *session = new DefaultAppsSettings(new KdeDefaultAppsBackend(paths)); endpoint->session = session;
            endpoint->ready = [session] { return session->isReady() && session->canApply(); };
            endpoint->stage = [session] { session->selectRole(DefaultAppRole::Terminal, "new-terminal.desktop"); };
        } else if (kind == Standards) {
            StandardShortcutsBackendOptions options; options.config = config("kdeglobals"); options.isolated = true;
            options.beforeCommit = hold; options.softDeadlineMs = 20;
            auto *session = new ShortcutsSettings(new KdeStandardShortcutsBackend(options)); endpoint->session = session;
            endpoint->ready = [session] { return session->isReady() && session->canApply(); };
            endpoint->stage = [session] {
                for (const auto &action : session->applied().actions) if (action.id.action == QLatin1String("Open")) {
                    session->selectShortcuts(action.id, {QKeySequence(QStringLiteral("Meta+Ctrl+F24"))}); return;
                }
            };
        } else {
            auto *runtime = new KeyboardRuntime; KeyboardBackendOptions options;
            options.isolated = true; options.inputConfig = config("kcminputrc"); options.layoutConfig = config("kxkbrc");
            options.workspaceConfig = config("plasmarc"); options.registryPath = config("evdev.xml");
            options.beforeSync = hold; options.writeBudgetMs = 20;
            auto *session = new KeyboardSettings(new KdeKeyboardBackend(runtime, options));
            endpoint->session = session; endpoint->service = runtime;
            endpoint->ready = [session] { return session->isReady() && session->canApply(); };
            endpoint->stage = [session, workspace] { if (workspace) session->selectValue("kbdLayoutChangedEnabled", false); else session->selectValue("RepeatDelay", 850); };
        }
        endpoint->hasService = !endpoint->service.isNull();
        endpoint->completed = std::make_unique<QSignalSpy>(endpoint->session.data(), &SettingsSession::applyFinished);
        endpoints.push_back(std::move(value)); return endpoint;
    }
private slots:
    void init() {
        home = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        QVERIFY(home.startsWith(qEnvironmentVariable("CONTROL_RC_PRIVATE_ROOT") + '/'));
        QVERIFY(data.startsWith(qEnvironmentVariable("CONTROL_RC_PRIVATE_ROOT") + '/'));
        QVERIFY(QDir(home).removeRecursively()); QVERIFY(QDir().mkpath(home)); seed();
    }
    void cleanup() {
        for (const auto &gate : gates) gate->released.release(32);
        QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(), 15000);
        for (const auto &endpoint : endpoints) delete endpoint->session;
        endpoints.clear(); gates.clear(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void orphanedProviderBlocksOverlapUntilActualTerminal_data() {
        QTest::addColumn<int>("first"); QTest::addColumn<int>("second");
        QTest::addColumn<bool>("fullGlobals"); QTest::addColumn<bool>("workspace");
        auto pair = [](const char *label, Kind a, Kind b, bool globals, bool workspace = false) {
            const auto prefix = QByteArray(label);
            QTest::newRow((prefix + "-forward").constData()) << int(a) << int(b) << globals << workspace;
            QTest::newRow((prefix + "-reverse").constData()) << int(b) << int(a) << globals << workspace;
        };
        pair("aero-nightlight-kwinrc", Aero, Filter, false);
        pair("aero-appearance-kwinrc", Aero, Appearance, false);
        pair("font-nightlight-kdeglobals", Font, Filter, true);
        pair("terminal-nightlight-kdeglobals", Apps, Filter, true);
        pair("standards-nightlight-kdeglobals", Standards, Filter, true);
        pair("aero-keyboard-kcminputrc", Aero, Keyboard, false);
        pair("aero-keyboard-plasmarc", Aero, Keyboard, false, true);
    }
    void orphanedProviderBlocksOverlapUntilActualTerminal() {
        QFETCH(int, first); QFETCH(int, second); QFETCH(bool, fullGlobals); QFETCH(bool, workspace);
        const auto held = std::make_shared<Gate>(); gates << held;
        auto *a = make(Kind(first), fullGlobals, workspace, held);
        auto *b = make(Kind(second), fullGlobals, workspace);
        QTRY_VERIFY_WITH_TIMEOUT(!a->session->isBusy() && !b->session->isBusy(), 10000);
        QVERIFY(a->ready()); QVERIFY(b->ready()); a->stage(); QVERIFY(a->session->isDirty());
        a->session->apply(); QTRY_VERIFY_WITH_TIMEOUT(held->entered.load(), 10000);
        QVERIFY(a->session->isBusy()); delete a->session; QVERIFY(!a->session);
        if (a->hasService) QVERIFY(a->service);
        QVERIFY(OperationManager::instance()->hasActiveJobs()); QCOMPARE(a->completed->size(), 0);
        b->stage(); QVERIFY(b->session->isDirty()); b->session->apply();
        QTRY_COMPARE_WITH_TIMEOUT(b->completed->size(), 1, 10000); QTRY_VERIFY(!b->session->isBusy());
        QVERIFY(!b->completed->at(0).at(0).toBool()); QVERIFY(b->session->hasConflict());
        QVERIFY(b->session->isDirty()); QVERIFY2(b->session->errorText().contains("owns") || b->session->errorText().contains("tracked"), qPrintable(b->session->errorText()));
        QVERIFY(OperationManager::instance()->hasActiveJobs()); if (a->hasService) QVERIFY(a->service);
        held->released.release(); QTRY_VERIFY_WITH_TIMEOUT(!OperationManager::instance()->hasActiveJobs(), 15000);
        if (a->hasService) QTRY_VERIFY(!a->service);
        QCOMPARE(a->completed->size(), 0);
        b->session->revert(); QTRY_VERIFY_WITH_TIMEOUT(!b->session->isBusy(), 10000);
        QVERIFY(b->ready()); QVERIFY(!b->session->hasConflict()); QVERIFY(!b->session->isDirty());
        b->stage(); QVERIFY(b->session->isDirty()); b->session->apply();
        QTRY_COMPARE_WITH_TIMEOUT(b->completed->size(), 2, 10000); QTRY_VERIFY(!b->session->isBusy());
        QVERIFY2(b->completed->at(1).at(0).toBool(), qPrintable(b->session->errorText()));
        QVERIFY(!b->session->isDirty()); QVERIFY(!b->session->hasConflict());
        for (const auto &name : {"kwinrc", "kdeglobals", "kcminputrc", "plasmarc", "smodrc", "Kvantum/kvantum.kvconfig", "kxkbrc"})
            QVERIFY2(bytes(config(QLatin1String(name))).contains("Keep=unowned"), name);
        QVERIFY(bytes(config("kdeglobals")).contains("BatteryChargeStopThreshold=80"));
        QVERIFY(bytes(config("fontconfig/fonts.conf")).contains("Preserve this unrelated comment"));
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (!qEnvironmentVariableIsSet("CONTROL_RC_FILE_JOBS_CHILD")) {
        QTemporaryDir sandbox; if (!sandbox.isValid()) return 2;
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto &pair : {qMakePair("XDG_CONFIG_HOME", "config"), qMakePair("XDG_CONFIG_DIRS", "defaults"),
                 qMakePair("XDG_DATA_HOME", "data"), qMakePair("XDG_STATE_HOME", "state"), qMakePair("XDG_CACHE_HOME", "cache")}) {
            const auto path = sandbox.filePath(QString::fromLatin1(pair.second)); if (!QDir().mkpath(path)) return 3;
            environment.insert(QString::fromLatin1(pair.first), path);
        }
        environment.insert("KDE_SKIP_KDERC", "1"); environment.insert("CONTROL_RC_FILE_JOBS_CHILD", "1");
        environment.insert("CONTROL_RC_PRIVATE_ROOT", sandbox.path());
        QProcess child; child.setProcessEnvironment(environment); child.setProcessChannelMode(QProcess::ForwardedChannels);
        QStringList args{"--", app.applicationFilePath()}; args << app.arguments().mid(1);
        child.start("/usr/bin/dbus-run-session", args);
        if (!child.waitForStarted(3000) || !child.waitForFinished(110000)) { child.kill(); child.waitForFinished(1000); return 4; }
        return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 5;
    }
    RcFileJobsIntegrationTest test; return QTest::qExec(&test, argc, argv);
}
#include "rc_file_jobs_integration_test.moc"
