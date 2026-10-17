#include "ui/MainWindow.h"
#include "ui/PageRegistry.h"
#include "ui/SettingsNavigationGuard.h"
#include "ui/pages/WindowBehaviorPage.h"
#include "ui/pages/PowerOptionsPage.h"
#include "ui/pages/DisplayPage.h"
#include "ui/pages/DesktopBackgroundPage.h"
#include "ui/pages/ThemeComponentsPage.h"
#include "ui/pages/FontSettingsPage.h"
#include "ui/pages/FontsPage.h"
#include "settings/WallpaperSettings.h"
#include "aero_theme_fixture.h"
#include "ui/pages/PersonalizationPage.h"
#include "settings/FontSettings.h"
#include "settings/UserFontService.h"
#include "ui/pages/UserAccountsPage.h"
#include "ui/pages/AeroEffectsPage.h"
#include "ui/pages/PackagePreviewPage.h"
#include "settings/AccountSettings.h"
#include "settings/AeroEffectSettings.h"
#include "packages/PackageSimulationService.h"
#include "settings/AppearanceSettings.h"
#include "settings/PowerPolicySettings.h"
#include "power_policy_fixture.h"
#include "settings/DisplaySettings.h"
#include "display_fixture.h"
#include "night_light_fixture.h"
#include "night_time_fixture.h"
#include "screen_lock_fixture.h"
#include "ui/pages/ScreenLockPage.h"
#include "ui/pages/AeroLoginPage.h"
#include "aero_login_fixture.h"
#include "ui/pages/NightTimePage.h"
#include "platform/location/NightTimeLocation.h"
#include "ui/pages/NightLightPage.h"
#include "network_fixture.h"
#include "audio_fixture.h"
#include "input_fixture.h"
#include "ui/pages/InputDevicesPage.h"
#include "ui/pages/KeyboardPage.h"
#include "ui/pages/DefaultAppsPage.h"
#include "ui/pages/ShortcutsPage.h"
#include "settings/ShortcutsSettings.h"
#include "settings/ShortcutImportCoordinator.h"
#include "ui/dialogs/ShortcutSchemeDialog.h"
#include "settings/KeyboardSettings.h"
#include "settings/DefaultAppsSettings.h"
#include "ui/dialogs/SoundDialog.h"
#include "ui/pages/NetworkSharingPage.h"
#include "ui/pages/NetworkConnectionsPage.h"
#include <AeroQt/stylesheet.h>
#include <QtTest>
#include <QAbstractButton>
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QImage>

class IntegrationAeroLoginBackend final:public AeroLoginBackend {
public:
    AeroLoginSnapshot current=aeroLoginFixture(),request;int writes=0,recoveries=0;quint64 pending=0;
    bool hold=false,failRead=false,saved=true,receipt=true;
    void readCurrent(quint64 token) override{emit readFinished(token,!failRead,current,failRead?QStringLiteral("Read failed"):QString());}
    void writeChanges(quint64 token,const AeroLoginSnapshot &,const AeroLoginSnapshot &requested) override{++writes;pending=token;request=requested;if(!hold)finish();}
    void recover(quint64 token,const AeroLoginSnapshot &) override{++recoveries;emit writeFinished(token,false,QStringLiteral("Fixture recovery held for review"));}
    void finish(){if(saved)current=savedAeroLoginFixture(request);emit writeFinished(pending,saved,saved?QString():QStringLiteral("Authorization canceled"),saved&&receipt?current.revision:QByteArray());}
};
class IntegrationScreenLockBackend final : public ScreenLockBackend {
public:
    ScreenLockSnapshot current = screenLockFixture(), request;
    int writes = 0; quint64 pending = 0;
    bool hold = false, failRead = false, notified = true;
    void readCurrent(quint64 token) override { emit readFinished(token, !failRead, current, failRead ? QStringLiteral("Read failed") : QString()); }
    void writeChanges(quint64 token, const ScreenLockSnapshot &, const ScreenLockSnapshot &requested) override {
        ++writes; request = requested; pending = token; if (!hold) finish();
    }
    void finish() { current = savedScreenLockFixture(request); emit writeFinished(pending, true, notified, {}); }
};

class IntegrationNightTimeBackend final : public NightTimeBackend {
public:
    NightTimeSnapshot current = nightTimeFixture(), request;
    int writes = 0; quint64 pending = 0;
    bool hold = false, failRead = false, notified = true;
    void readCurrent(quint64 token) override { emit readFinished(token, !failRead, current, failRead ? QStringLiteral("Read failed") : QString()); }
    void writeChanges(quint64 token, const NightTimeSnapshot &, const NightTimeSnapshot &requested) override {
        ++writes; request = requested; pending = token; if (!hold) finish();
    }
    void finish() { current = savedNightTimeFixture(request); emit writeFinished(pending, true, notified, {}); }
};

class IntegrationNightLightBackend final : public NightLightBackend {
public:
    NightLightSnapshot current = nightLightFixture(), request;
    int writes = 0, previews = 0, stops = 0;
    quint64 pending = 0;
    bool hold = false, failRead = false, notified = true;
    void readCurrent(quint64 token) override { emit readFinished(token, !failRead, current, failRead ? QStringLiteral("Read failed") : QString()); }
    void writeChanges(quint64 token, const NightLightSnapshot &, const NightLightSnapshot &requested) override {
        ++writes; request = requested; pending = token; if (!hold) finish();
    }
    void finish() { current = savedNightLightFixture(request); emit writeFinished(pending, true, notified, {}); }
    void preview(quint64 token, int, const QString &, quint64) override { ++previews; emit previewFinished(token, true, false, {}); }
    void stopPreview(quint64 token, const QString &, quint64) override { ++stops; emit previewFinished(token, true, true, {}); }
};

class IntegrationWallpaperBackend final : public WallpaperBackend {
public:
    WallpaperState current;
    IntegrationWallpaperBackend() {
        WallpaperDesktop desktop; desktop.id = 1; desktop.screen = 0;
        desktop.plugin = QStringLiteral("org.kde.image"); current.desktops << desktop;
    }
    void readCurrent() override { emit readFinished(true, current, {}); }
    void writePicture(const WallpaperDesktop &, const QString &image, int mode) override {
        current.desktops.first().image = image; current.desktops.first().fillMode = mode;
        emit writeFinished(true, {});
    }
};
class IntegrationFontBackend final : public FontBackend {
public:
    FontState current;
    int writes = 0;
    IntegrationFontBackend() {
        current.writable = current.reloadAvailable = true;
        for (auto &font : current.preferences.fonts) font = QFont(QStringLiteral("Noto Sans"), 10);
        current.preferences.fonts[1] = QFont(QStringLiteral("DejaVu Sans Mono"), 10);
        current.preferences.antialias = QStringLiteral("true");
        current.preferences.hinting = QStringLiteral("hintslight"); current.preferences.subpixel = QStringLiteral("rgb");
    }
    void readCurrent() override { emit readFinished(true, current, {}); }
    void writePreferences(const FontState &, const FontPreferences &preferences) override {
        ++writes; current.preferences = preferences; emit writeFinished(true, {});
    }
};
class IntegrationAccountBackend final : public AccountBackend {
public:
    AccountState current;
    int writes = 0;
    IntegrationAccountBackend() {
        current.uid = 1000; current.userName = QStringLiteral("fixture"); current.realName = QStringLiteral("Windows User");
        current.objectPath = QStringLiteral("/org/freedesktop/Accounts/User1000"); current.serviceOwner = QStringLiteral(":1.100"); current.editable = true;
    }
    void readCurrent(quint64 token) override { emit readFinished(token, true, current, {}); }
    void authorize(quint64 token) override { emit authorizationFinished(token, AccountOutcome::Success, {}); }
    void cancelAuthorization(quint64) override {}
    void writeName(quint64 token, const AccountState &, const QString &name) override {
        ++writes; current.realName = name; emit writeFinished(token, AccountOutcome::Success, {});
    }
};
class IntegrationAeroBackend final : public AeroEffectBackend {
public:
    AeroEffectState current{3, true, true, true, true, {}};
    void readCurrent() override { emit readFinished(true, current, {}); }
    void writeStrength(int, int value) override { current.blurStrength = value; emit writeFinished(true, {}); }
};
class IntegrationSimulationBackend final : public PackageSimulationBackend {
public:
    quint64 request = 0;
    bool isAvailable() const override { return true; }
    QString unavailableReason() const override { return {}; }
    void start(quint64 id) override { request = id; }
    void cancel(quint64 id) override { PackageSimulationProcessResult result; result.cancelled = true; emit finished(id, result); }
    void complete() {
        PackageSimulationProcessResult result; result.normalExit = true; result.exitCode = 0;
        result.standardOutput = "Reading package lists...\nBuilding dependency tree...\nReading state information...\n"
            "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n"
            "Inst fixture-package [1.0] (1.1 Ubuntu:26.04/stable [amd64])\nConf fixture-package (1.1 Ubuntu:26.04/stable [amd64])\n";
        emit finished(request, result);
    }
};

class IntegrationAppearanceBackend final : public AppearanceBackend {
public:
    QString action = QStringLiteral("Maximize");
    int writes = 0;
    void readCurrent() override { emit readFinished(true, {action, true, true, {}}, {}); }
    void writeAction(const QString &, const QString &requested) override {
        ++writes; action = requested; emit writeFinished(true, {});
    }
};
class IntegrationPowerProfiles final : public PowerProfileBackend {
public:
    QStringList availableProfiles(QString *) override { return {"balanced", "performance"}; }
    QString activeProfile(QString *) override { return "balanced"; }
    void setActiveProfile(const QString &, Completion completion) override { completion({}); }
};
class IntegrationPowerBackend final : public PowerPolicyBackend {
public:
    PowerPolicySnapshot current, requested;
    IntegrationPowerBackend() {
        current.serviceAvailable = true;
        current.owner = ":1.42"; current.capabilityRevision = "caps1";
        current.batteryLevelsEditable = current.batteryCriticalActionEditable = true;
        current.batteryCriticalActions = {{0, "Do nothing"}, {1, "Sleep"}, {2, "Hibernate"}, {8, "Shut down"}};
        for (const QString &id : {QStringLiteral("AC"), QStringLiteral("Battery"), QStringLiteral("LowBattery")}) {
            PowerPolicyProfile profile; profile.id = id;
            profile.displayEditable = profile.sleepEditable = true;
            profile.idleActions = current.batteryCriticalActions;
            profile.powerButtonActions = {{0, "Do nothing"}, {1, "Sleep"}, {2, "Hibernate"}, {8, "Shut down"}, {16, "Show logout screen"}, {32, "Lock screen"}, {64, "Turn off screen"}};
            profile.lidActions = {{0, "Do nothing"}, {1, "Sleep"}, {2, "Hibernate"}, {8, "Shut down"}, {32, "Lock screen"}, {64, "Turn off screen"}};
            profile.sleepModes = {{1, "Suspend to RAM"}, {2, "Hybrid suspend"}, {3, "Suspend then hibernate"}};
            profile.dimEditable = profile.brightnessEditable = profile.keyboardBrightnessEditable = true;
            profile.lockedDisplayEditable = profile.lockBeforeDisplayOffEditable = true;
            profile.powerButtonEditable = profile.powerDownEditable = profile.lidEditable = true;
            profile.lidExternalMonitorEditable = profile.sleepModeEditable = true;
            current.profiles << profile;
        }
        seedPowerPolicyMetadata(current);
    }
    void readCurrent() override { emit readFinished(true, current, {}); }
    void writePolicy(const PowerPolicySnapshot &, const PowerPolicySnapshot &value) override { requested = value; }
    void cancel() override {}
    void finish() { current = savedPowerPolicyRequest(requested); emit writeFinished(true, true, false, {}); }
};
class IntegrationDisplayBackend final : public DisplayBackend {
public:
    DisplaySnapshot current = displayFixture(), baseline = current;
    bool rollbackAvailable() const override { return true; }
    void readCurrent() override { emit readFinished(true, current, {}); }
    void beginPreview(const DisplaySnapshot &before, const DisplaySnapshot &requested) override {
        baseline = before; current = requested; emit previewReady(current, 15);
    }
    void keepPreview() override { emit transactionFinished(true, false, current, {}); }
    void revertPreview() override { current = baseline; emit transactionFinished(false, true, current, {}); }
};

class IntegrationKeyboardBackend final : public KeyboardBackend {
public:
    KeyboardSnapshot current;
    QVariantMap requested;
    quint64 pending = 0;
    int writes = 0;
    bool hold = false;
    IntegrationKeyboardBackend() {
        current.available = current.writable = current.reloadAvailable = true;
        current.owner = ":1.42"; current.version = "6.6.6"; current.revision = "before";
        current.values = KeyboardTypes::defaults(); current.registry.valid = true;
        current.registry.revision = "registry"; current.registry.models = {{"pc104", "Standard keyboard"}};
        current.registry.layouts = {{"us", "English (US)", {}}};
    }
    void readCurrent(quint64 token) override { emit readFinished(token, true, current, {}); }
    void writePreferences(quint64 token, const KeyboardSnapshot &, const QVariantMap &values) override {
        ++writes; pending = token; requested = values; if (!hold) finish();
    }
    void finish() {
        current.values = requested; current.revision = "after";
        emit writeFinished(pending, KeyboardOutcome::Success, current, {});
    }
};
class IntegrationDefaultAppsBackend final : public DefaultAppsBackend {
public:
    DefaultAppsSnapshot current;
    DefaultAppsPreferences requested;
    quint64 pending = 0;
    int writes = 0;
    bool hold = false;
    IntegrationDefaultAppsBackend() {
        current.writable = true; current.revision = "before";
        current.mimeTypes = {"x-scheme-handler/http", "x-scheme-handler/https"};
        DefaultApplication first; first.id = "first.desktop"; first.name = "First Browser";
        first.exec = "/usr/bin/fixture-first %u"; first.mimeTypes = current.mimeTypes;
        first.identity = "first"; auto second = first; second.id = "second.desktop";
        second.name = "Second Browser"; second.identity = "second";
        current.applications = {first, second};
        current.preferences.mimeDefaults = {{"x-scheme-handler/http", first.id}, {"x-scheme-handler/https", first.id}};
    }
    void readCurrent(quint64 token) override { emit readFinished(token, true, current, {}); }
    void writeChanges(quint64 token, const DefaultAppsSnapshot &, const DefaultAppsPreferences &values) override {
        ++writes; pending = token; requested = values; if (!hold) finish();
    }
    void finish() {
        current.preferences = requested; current.revision = "after";
        emit writeFinished(pending, DefaultAppsOutcome::Success, current, {});
    }
};

class IntegrationShortcutsBackend final : public ShortcutsBackend {
public:
    ShortcutsSnapshot current;
    ShortcutChanges requested;
    quint64 pending = 0;
    int writes = 0;
    bool hold = false;
    const ShortcutActionId actionId{QStringLiteral("kwin"), QStringLiteral("default"), QStringLiteral("Minimize")};
    IntegrationShortcutsBackend() {
        current.available = current.writable = true; current.owner = ":1.42"; current.interfaceSignature = "fixture-interface";
        ShortcutAction action; action.id = actionId; action.componentFriendly = "Window Manager";
        action.contextFriendly = "Default context"; action.friendlyName = "Minimize Window";
        action.current = {QKeySequence("Meta+F5")}; action.defaults = {QKeySequence("Meta+F6")};
        current.actions = {action}; current.revision = ShortcutsTypes::revision(current);
    }
    void readCurrent(quint64 token) override { emit readFinished(token, true, current, {}); }
    void writeChanges(quint64 token, const ShortcutsSnapshot &, const ShortcutChanges &values) override {
        ++writes; pending = token; requested = values; if (!hold) finish();
    }
    void finish() {
        for (auto &action : current.actions) if (requested.contains(action.id)) action.current = requested.value(action.id);
        current.revision = ShortcutsTypes::revision(current);
        emit writeFinished(pending, ShortcutsOutcome::Success, current, {});
    }
};

class IntegrationCustomShortcutsBackend final : public CustomShortcutsBackend {
public:
    int writes = 0;
    void readCurrent(quint64 token) override {
        CustomShortcutsSnapshot state; state.available = state.writable = true;
        state.owner = QStringLiteral(":1.42"); state.interfaceSignature = QStringLiteral("fixture-custom-interface");
        state.revision = CustomShortcutsTypes::revision(state); emit readFinished(token, true, state, {});
    }
    void writeChanges(quint64 token, const CustomShortcutsSnapshot &expected, const CustomShortcutChanges &) override {
        ++writes; emit writeFinished(token, ShortcutsOutcome::Failed, expected, QStringLiteral("Unexpected custom write"));
    }
};

class KdeIntegrationTest final : public QObject {
    Q_OBJECT
    QTemporaryDir environment;
    QByteArray oldPath, oldConfig;
    static void answer(const QString &label) {
        QTimer::singleShot(0, qApp, [label] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                for (auto *button : box->buttons()) {
                    QString text = button->text(); text.remove('&');
                    if (text == label) { button->click(); return; }
                }
        });
    }
    static void inject(MainWindow &window, QWidget *page, SettingsSession *session, PageId id) {
        const auto path = PageRegistry::pathFor(id);
        window.m_history = {QString(), path}; window.m_historyIndex = 1;
        window.m_scroll->setWidget(page);
        window.setCrumbTrail(path.split('/'));
        window.registerSettingsSession(session, path.section('/', -1));
        window.updateNavButtons();
    }
private slots:
    void initTestCase() {
        QVERIFY(environment.isValid());
        oldPath = qgetenv("PATH"); oldConfig = qgetenv("XDG_CONFIG_HOME");
        qputenv("PATH", environment.path().toLocal8Bit());
        qputenv("XDG_CONFIG_HOME", environment.path().toLocal8Bit());
        QCoreApplication::setOrganizationName(QStringLiteral("linux-control-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("kde-integration"));
        QFile reader(environment.filePath(QStringLiteral("kreadconfig6")));
        QVERIFY(reader.open(QIODevice::WriteOnly));
        reader.write("#!/bin/sh\nprintf 'BreezeLight\\n'\n"); reader.close();
        QVERIFY(reader.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    void cleanupTestCase() { qputenv("PATH", oldPath); qputenv("XDG_CONFIG_HOME", oldConfig); }
    void newAppearancePageUsesNavigationGuard() {
        auto *backend = new IntegrationAppearanceBackend;
        AppearanceSettings session(backend);
        QTRY_VERIFY(!session.isBusy());
        MainWindow window(nullptr, false); window.show();
        QTRY_VERIFY(!window.hasBackgroundWork());
        inject(window, new WindowBehaviorPage(new QScrollArea, &session, nullptr), &session, PageId::WindowBehavior);
        QCOMPARE(window.currentSettingsSession(), &session);
        session.selectAction(QStringLiteral("Minimize"));
        answer(QStringLiteral("Cancel")); window.goBack();
        QCOMPARE(window.m_historyIndex, 1); QVERIFY(session.isDirty()); QCOMPARE(backend->writes, 0);
        answer(QStringLiteral("Apply")); window.goBack();
        QTRY_COMPARE(window.m_historyIndex, 0);
        QCOMPARE(backend->writes, 1); QVERIFY(!session.isDirty());
        QVERIFY(window.m_jobLabel->text().contains(QStringLiteral("Window Behavior")));
    }
    void screenLockUsesSharedNavigationAndRetainsFailedDiscard() {
        auto *backend = new IntegrationScreenLockBackend; ScreenLockSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); MainWindow window(nullptr, false); window.show();
        inject(window, new ScreenLockPage(new QScrollArea, &session, ScreenLockAppearance::Evidence{}, nullptr), &session, PageId::ScreenLock);
        QCOMPARE(window.currentSettingsSession(), &session);
        session.selectTimeout(10.0);
        answer(QStringLiteral("Cancel")); window.goBack(); QCOMPARE(window.m_historyIndex, 1);
        QVERIFY(session.isDirty()); QCOMPARE(backend->writes, 0);
        backend->failRead = true; answer(QStringLiteral("Discard")); window.goBack();
        QTRY_VERIFY(!window.m_navigationGuard->pending()); QCOMPARE(window.m_historyIndex, 1); QVERIFY(session.isDirty());
        backend->failRead = false; answer(QStringLiteral("Discard")); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0);
        QVERIFY(!session.isDirty()); QCOMPARE(backend->writes, 0);
        inject(window, new ScreenLockPage(new QScrollArea, &session, ScreenLockAppearance::Evidence{}, nullptr), &session, PageId::ScreenLock);
        session.selectTimeout(10.0); answer(QStringLiteral("Apply")); window.goBack();
        QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!session.isDirty()); QCOMPARE(backend->writes, 1);
    }
    void aeroLoginNavigationPinsConcreteReviewAndFailedDiscardKeepsDraft(){
        auto *backend=new IntegrationAeroLoginBackend;AeroLoginSettings session(backend);QTRY_VERIFY(!session.isBusy());MainWindow window(nullptr,false);window.show();
        inject(window,new AeroLoginPage(new QScrollArea,&session,nullptr),&session,PageId::AeroLogin);QCOMPARE(window.currentSettingsSession(),&session);
        session.removeImage();answer("Cancel");window.goBack();QCOMPARE(window.m_historyIndex,1);QCOMPARE(backend->writes,0);
        backend->failRead=true;answer("Discard");window.goBack();QTRY_VERIFY(!window.m_navigationGuard->pending());QVERIFY(session.isDirty());QCOMPARE(window.m_historyIndex,1);backend->failRead=false;session.revert();session.removeImage();
        bool reviewed=false;QTimer::singleShot(0,&window,[&]{auto *box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget());QVERIFY(box);QVERIFY(box->text().contains("Remove the background override"));QVERIFY(box->text().contains("fallback"));QVERIFY(box->text().contains("Administrative authorization"));reviewed=true;for(auto *button:box->buttons())if(button->text()=="Apply")button->click();});
        window.goBack();QVERIFY(reviewed);QTRY_COMPARE(window.m_historyIndex,0);QCOMPARE(backend->writes,1);QVERIFY(!session.isDirty());
    }
    void aeroLoginNavigationRejectsDraftChangedInsideReview(){
        auto *backend=new IntegrationAeroLoginBackend;AeroLoginSettings session(backend);QTRY_VERIFY(!session.isBusy());MainWindow window(nullptr,false);window.show();inject(window,new AeroLoginPage(nullptr,&session,nullptr),&session,PageId::AeroLogin);session.selectValue(AeroLoginKey::StartupAnimation,true);
        QTimer::singleShot(0,&window,[&]{auto *box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget());QVERIFY(box);session.removeImage();for(auto *button:box->buttons())if(button->text()=="Apply")button->click();});window.goBack();QCOMPARE(window.m_historyIndex,1);QCOMPARE(backend->writes,0);QVERIFY(session.isDirty());
        answer("Discard");window.goBack();QTRY_COMPARE(window.m_historyIndex,0);
    }
    void aeroLoginJobAndPreparedPictureSurviveDestroyedViewAndBlockSilentClose(){
        auto *backend=new IntegrationAeroLoginBackend;backend->hold=true;AeroLoginSettings session(backend);QTRY_VERIFY(!session.isBusy());MainWindow window(nullptr,false);window.show();inject(window,new AeroLoginPage(nullptr,&session,nullptr),&session,PageId::AeroLogin);session.selectValue(AeroLoginKey::StartupAnimation,true);session.apply();QVERIFY(session.isBusy());
        window.m_scroll->setWidget(new QWidget);QVERIFY(window.hasBackgroundWork());answer("OK");QVERIFY(!window.close());backend->finish();QTRY_VERIFY(!window.hasBackgroundWork());QVERIFY(!session.isDirty());
        session.setProperty("aeroLoginPendingImage",QVariant::fromValue(aeroLoginImageFixture(Qt::yellow)));QVERIFY(session.isDirty());answer("Cancel");QVERIFY(!window.close());QVERIFY(session.hasImageReview());
        answer("Discard");window.close();QTRY_VERIFY(!window.isVisible());QVERIFY(!session.isDirty());QVERIFY(!session.hasImageReview());
    }
    void screenLockJobSurvivesViewDestructionAndBlocksClose() {
        auto *backend = new IntegrationScreenLockBackend; backend->hold = true; ScreenLockSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); MainWindow window(nullptr, false); window.show();
        inject(window, new ScreenLockPage(new QScrollArea, &session, ScreenLockAppearance::Evidence{}, nullptr), &session, PageId::ScreenLock);
        session.selectTimeout(10.0); session.apply();
        QVERIFY(session.isBusy()); QVERIFY(window.hasBackgroundWork()); window.m_scroll->setWidget(new QWidget);
        QVERIFY(session.isBusy()); QVERIFY(window.hasBackgroundWork()); answer(QStringLiteral("OK")); QVERIFY(!window.close());
        backend->finish(); QTRY_VERIFY(!window.hasBackgroundWork()); QVERIFY(!session.isDirty());
        QVERIFY(window.m_jobLabel->text().contains(QStringLiteral("Screen Locking"))); QVERIFY(window.close());
    }
    void screenLockNavigationWaitsThroughSoftDeadlineAndReadback() {
        auto *backend = new IntegrationScreenLockBackend; backend->hold = true; ScreenLockSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); MainWindow window(nullptr, false); window.show();
        inject(window, new ScreenLockPage(nullptr, &session, ScreenLockAppearance::Evidence{}, nullptr), &session, PageId::ScreenLock);
        session.selectTimeout(10.0); answer(QStringLiteral("Apply")); window.goBack();
        QTRY_COMPARE(backend->writes, 1); QVERIFY(session.isBusy()); QVERIFY(window.m_navigationGuard->pending());
        emit backend->operationPending(backend->pending, QStringLiteral("Saving is taking longer; the operation continues."));
        QCOMPARE(window.m_historyIndex, 1); QVERIFY(window.m_navigationGuard->pending());
        QVERIFY(session.errorText().isEmpty()); QVERIFY(session.isDirty());
        auto *message = window.findChild<QLabel *>(QStringLiteral("screenLockMessage")); QVERIFY(message);
        QVERIFY(message->text().contains(QStringLiteral("taking longer")));
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("screenLockApply"))->isEnabled());
        QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("screenLockRevert"))->isEnabled());
        backend->finish(); QTRY_COMPARE(window.m_historyIndex, 0);
        QVERIFY(!session.isBusy()); QVERIFY(!session.isDirty()); QVERIFY(!session.hasConflict());
        QVERIFY(!window.m_navigationGuard->pending()); QVERIFY(window.close());
    }
    void nightTimeUsesSharedNavigationAndRetainsFailedDiscard() {
        auto *backend = new IntegrationNightTimeBackend; NightTimeSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); MainWindow window(nullptr, false); window.show();
        inject(window, new NightTimePage(new QScrollArea, &session, new NightTimeLocation(NightTimeLocation::Factory{}), nullptr), &session, PageId::NightTime);
        QCOMPARE(window.currentSettingsSession(), &session);
        session.selectValue(NightTimeKey::Source, 1);
        answer(QStringLiteral("Cancel")); window.goBack(); QCOMPARE(window.m_historyIndex, 1);
        QVERIFY(session.isDirty()); QCOMPARE(backend->writes, 0);
        backend->failRead = true; answer(QStringLiteral("Discard")); window.goBack();
        QTRY_VERIFY(!window.m_navigationGuard->pending()); QCOMPARE(window.m_historyIndex, 1); QVERIFY(session.isDirty());
        backend->failRead = false; answer(QStringLiteral("Discard")); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0);
        QVERIFY(!session.isDirty()); QCOMPARE(backend->writes, 0);
        inject(window, new NightTimePage(new QScrollArea, &session, new NightTimeLocation(NightTimeLocation::Factory{}), nullptr), &session, PageId::NightTime);
        session.selectValue(NightTimeKey::Source, 1); answer(QStringLiteral("Apply")); window.goBack();
        QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!session.isDirty()); QCOMPARE(backend->writes, 1);
    }
    void nightTimeJobSurvivesViewDestructionAndBlocksClose() {
        auto *backend = new IntegrationNightTimeBackend; backend->hold = true; NightTimeSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); MainWindow window(nullptr, false); window.show();
        inject(window, new NightTimePage(new QScrollArea, &session, new NightTimeLocation(NightTimeLocation::Factory{}), nullptr), &session, PageId::NightTime);
        session.selectValue(NightTimeKey::Source, 1); session.apply();
        emit backend->operationPending(backend->pending, QStringLiteral("Saving continues in the background."));
        QVERIFY(session.isDirty()); QVERIFY(session.errorText().isEmpty());
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("nightTimeMessage"))->text().contains(QStringLiteral("continues")));
        QVERIFY(session.isBusy()); QVERIFY(window.hasBackgroundWork()); window.m_scroll->setWidget(new QWidget);
        QVERIFY(session.isBusy()); QVERIFY(window.hasBackgroundWork()); answer(QStringLiteral("OK")); QVERIFY(!window.close());
        backend->finish(); QTRY_VERIFY(!window.hasBackgroundWork()); QVERIFY(!session.isDirty());
        QVERIFY(window.m_jobLabel->text().contains(QStringLiteral("Day and Night"))); QVERIFY(window.close());
    }
    void nightLightUsesSharedNavigationAndRetainsFailedDiscard() {
        auto *backend = new IntegrationNightLightBackend; NightLightSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); MainWindow window(nullptr, false); window.show();
        inject(window, new NightLightPage(new QScrollArea, &session, nullptr), &session, PageId::NightLight);
        QCOMPARE(window.currentSettingsSession(), &session);
        session.selectValue(NightLightKey::NightTemperature, 4000);
        answer(QStringLiteral("Cancel")); window.goBack(); QCOMPARE(window.m_historyIndex, 1);
        QVERIFY(session.isDirty()); QCOMPARE(backend->writes, 0);
        backend->failRead = true; answer(QStringLiteral("Discard")); window.goBack();
        QTRY_VERIFY(!window.m_navigationGuard->pending()); QCOMPARE(window.m_historyIndex, 1); QVERIFY(session.isDirty());
        backend->failRead = false; answer(QStringLiteral("Discard")); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0);
        QVERIFY(!session.isDirty()); QCOMPARE(backend->writes, 0);
        inject(window, new NightLightPage(new QScrollArea, &session, nullptr), &session, PageId::NightLight);
        session.selectValue(NightLightKey::NightTemperature, 4100); answer(QStringLiteral("Apply")); window.goBack();
        QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!session.isDirty()); QCOMPARE(backend->writes, 1);
    }
    void nightLightJobSurvivesViewDestructionAndBlocksClose() {
        auto *backend = new IntegrationNightLightBackend; backend->hold = true; NightLightSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); MainWindow window(nullptr, false); window.show();
        inject(window, new NightLightPage(new QScrollArea, &session, nullptr), &session, PageId::NightLight);
        session.selectValue(NightLightKey::NightTemperature, 4000); session.apply();
        emit backend->operationPending(backend->pending, QStringLiteral("Saving continues in the background."));
        QVERIFY(session.isDirty()); QVERIFY(session.errorText().isEmpty());
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("nightLightMessage"))->text().contains(QStringLiteral("continues")));
        QVERIFY(session.isBusy()); QVERIFY(window.hasBackgroundWork()); window.m_scroll->setWidget(new QWidget);
        QVERIFY(session.isBusy()); QVERIFY(window.hasBackgroundWork()); answer(QStringLiteral("OK")); QVERIFY(!window.close());
        backend->finish(); QTRY_VERIFY(!window.hasBackgroundWork()); QVERIFY(!session.isDirty());
        QVERIFY(window.m_jobLabel->text().contains(QStringLiteral("Night Light"))); QVERIFY(window.close());
    }
    void powerWorkSurvivesDeletedViewAndBlocksExit() {
        auto *backend = new IntegrationPowerBackend;
        PowerPolicySettings session(backend);
        PowerProfileService profiles(std::make_unique<IntegrationPowerProfiles>());
        QTRY_VERIFY(!session.isBusy());
        MainWindow window(nullptr, false); window.show();
        QTRY_VERIFY(!window.hasBackgroundWork());
        inject(window, new PowerOptionsPage(new QScrollArea, &session, &profiles, nullptr), &session, PageId::PowerOptions);
        QCOMPARE(window.currentSettingsSession(), &session);
        session.selectDisplayTimeout(QStringLiteral("AC"), 300); session.apply();
        QVERIFY(session.isBusy()); QVERIFY(window.hasBackgroundWork());
        // Force view destruction to verify ownership independently of the guard.
        window.m_scroll->setWidget(new QWidget);
        QVERIFY(session.isBusy()); QVERIFY(window.hasBackgroundWork());
        answer(QStringLiteral("OK")); QVERIFY(!window.close());
        backend->finish();
        QVERIFY(!session.isBusy()); QVERIFY(!window.hasBackgroundWork());
        QVERIFY(window.m_jobLabel->text().contains(QStringLiteral("Power Options")));
        QVERIFY(window.close());
    }
    void displayNavigationWaitsForConfirmedOutcome() {
        DisplaySettings session(new IntegrationDisplayBackend);
        QTRY_VERIFY(!session.isBusy());
        MainWindow window(nullptr, false); window.show();
        QTRY_VERIFY(!window.hasBackgroundWork());
        inject(window, new DisplayPage(new QScrollArea, &session, nullptr), &session, PageId::Display);
        QCOMPARE(window.currentSettingsSession(), &session);
        session.selectMode(QStringLiteral("2"));
        answer(QStringLiteral("Apply")); window.goBack();
        QVERIFY(session.awaitingConfirmation()); QVERIFY(window.m_navigationGuard->pending());
        QCOMPARE(window.m_historyIndex, 1); QVERIFY(window.hasBackgroundWork());
        session.keep();
        QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!window.hasBackgroundWork());
        QVERIFY(window.m_jobLabel->text().contains(QStringLiteral("Screen Resolution")));
    }
    void newFontEditorUsesApplyDiscardCancelNavigation() {
        auto *backend = new IntegrationFontBackend; FontSettings font(backend);
        QTRY_VERIFY(!font.isBusy()); MainWindow window(nullptr, false); window.show();
        QTRY_VERIFY(!window.hasBackgroundWork());
        inject(window, new FontSettingsPage(nullptr, &font, nullptr), &font, PageId::FontSettings);
        QCOMPARE(window.currentSettingsSession(), &font);
        font.selectFont(0, QFont(QStringLiteral("Noto Sans"), 12));
        answer(QStringLiteral("Cancel")); window.goBack(); QCOMPARE(window.m_historyIndex, 1);
        QVERIFY(font.isDirty()); QCOMPARE(backend->writes, 0);
        answer(QStringLiteral("Apply")); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0);
        QCOMPARE(backend->writes, 1); QVERIFY(!font.isDirty());
        inject(window, new FontSettingsPage(nullptr, &font, nullptr), &font, PageId::FontSettings);
        font.selectFont(0, QFont(QStringLiteral("Noto Sans"), 14));
        answer(QStringLiteral("Discard")); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0);
        QCOMPARE(backend->writes, 1); QVERIFY(!font.isDirty());
    }
    void wallpaperAndComponentsParticipateInSharedGuard() {
        WallpaperSettings wallpaper(new IntegrationWallpaperBackend);
        AeroThemeSettings components(new FixtureAeroThemeBackend);
        QTRY_VERIFY(!wallpaper.isBusy() && !components.isBusy()); MainWindow window(nullptr, false); window.show();
        QTRY_VERIFY(!window.hasBackgroundWork());
        inject(window, new DesktopBackgroundPage(nullptr, &wallpaper, nullptr), &wallpaper, PageId::DesktopBackground);
        QCOMPARE(window.currentSettingsSession(), &wallpaper); wallpaper.selectPicture(QStringLiteral("/staged.png"));
        answer(QStringLiteral("Discard")); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!wallpaper.isDirty());
        inject(window, new ThemeComponentsPage(nullptr, &components, nullptr), &components, PageId::ThemeComponents);
        QCOMPARE(window.currentSettingsSession(), &components); components.selectValue(QStringLiteral("aeroIntensity"), 60);
        answer(QStringLiteral("Apply")); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!components.isDirty());
    }
    void networkProfileUsesApplyDiscardCancelAndConfirmation() {
        auto *backend = new IntegrationNetworkBackend; NetworkSettings network(backend);
        QTRY_VERIFY(!network.isBusy()); MainWindow window(nullptr, false); window.show();
        inject(window, new NetworkConnectionsPage(nullptr, &network, nullptr), &network, PageId::NetworkConnections);
        QCOMPARE(window.currentSettingsSession(), &network);
        auto preferences = network.stagedPreferences(); preferences.id = QStringLiteral("Edited Ethernet");
        network.stagePreferences(preferences);
        answer(QStringLiteral("Cancel")); window.goBack(); QCOMPARE(window.m_historyIndex, 1);
        QVERIFY(network.isDirty()); QCOMPARE(backend->writes, 0);
        answer(QStringLiteral("Discard")); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0);
        QVERIFY(!network.isDirty()); QCOMPARE(backend->writes, 0);
        inject(window, new NetworkConnectionsPage(nullptr, &network, nullptr), &network, PageId::NetworkConnections);
        network.stagePreferences(preferences);
        answer(QStringLiteral("Apply")); window.goBack(); QVERIFY(window.m_navigationGuard->pending());
        QCOMPARE(backend->writes, 1); QCOMPARE(window.m_historyIndex, 1);
        backend->preview(); QVERIFY(network.awaitingConfirmation()); network.keep();
        QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!network.isDirty()); QVERIFY(!window.hasBackgroundWork());
    }
    void wirelessScanSurvivesNavigationAndPreventsEarlyExit() {
        auto *backend = new IntegrationNetworkBackend;
        backend->current.devices.first().type = 2; backend->holdScan = true;
        NetworkSettings network(backend); QTRY_VERIFY(!network.isBusy());
        MainWindow window(nullptr, false); window.show();
        inject(window, new NetworkConnectionsPage(nullptr, &network, nullptr), &network, PageId::NetworkConnections);
        network.scan(backend->current.devices.first().path);
        QVERIFY(network.isScanning()); QVERIFY(window.hasBackgroundWork());
        window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0);
        QVERIFY(network.isScanning()); QVERIFY(window.hasBackgroundWork());
        QVERIFY(window.m_jobLabel->text().contains(QStringLiteral("Scanning wireless networks")));
        answer(QStringLiteral("OK")); QVERIFY(!window.close());
        backend->finishScan(); QVERIFY(!network.isScanning()); QVERIFY(!window.hasBackgroundWork());
        QVERIFY(window.close());
    }
    void networkOperationOutlivesBothViewsAndRetainsItsOutcome() {
        auto *backend = new IntegrationNetworkBackend; NetworkSettings network(backend);
        QTRY_VERIFY(!network.isBusy()); MainWindow window(nullptr, false); window.show();
        inject(window, new NetworkSharingPage(nullptr, &network, nullptr), &network, PageId::NetworkSharing);
        QCOMPARE(window.currentSettingsSession(), &network);
        auto preferences = network.stagedPreferences(); preferences.id = QStringLiteral("Kept Ethernet");
        network.stagePreferences(preferences); network.apply(); QVERIFY(network.isBusy());
        delete window.m_scroll->takeWidget(); window.m_scroll->setWidget(new QWidget);
        QVERIFY(window.hasBackgroundWork()); backend->preview(); network.keep();
        QVERIFY(!window.hasBackgroundWork()); QVERIFY(!network.resultText().isEmpty());
        QVERIFY(window.m_jobDetails->isEnabled());
        inject(window, new NetworkConnectionsPage(nullptr, &network, nullptr), &network, PageId::NetworkConnections);
        QCOMPARE(window.currentSettingsSession(), &network);
        QCOMPARE(network.stagedPreferences().id, QStringLiteral("Kept Ethernet"));
        QVERIFY(window.close());
    }
    void accountAndAeroUseSharedNavigationGuard() {
        auto *backend = new IntegrationAccountBackend; AccountSettings account(backend);
        AeroEffectSettings aero(new IntegrationAeroBackend);
        QTRY_VERIFY(!account.isBusy() && !aero.isBusy()); MainWindow window(nullptr, false); window.show();
        QTRY_VERIFY(!window.hasBackgroundWork());
        inject(window, new UserAccountsPage(nullptr, &account, nullptr), &account, PageId::UserAccounts);
        QCOMPARE(window.currentSettingsSession(), &account); account.selectName(QStringLiteral("New Display Name"));
        answer(QStringLiteral("Cancel")); window.goBack(); QCOMPARE(window.m_historyIndex, 1); QCOMPARE(backend->writes, 0);
        answer(QStringLiteral("Apply")); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0); QCOMPARE(backend->writes, 1);
        inject(window, new AeroEffectsPage(nullptr, &aero, nullptr), &aero, PageId::AeroEffects);
        QCOMPARE(window.currentSettingsSession(), &aero); aero.selectStrength(8);
        answer(QStringLiteral("Discard")); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0); QCOMPARE(aero.stagedStrength(), 3);
    }
    void packageSimulationSurvivesNavigationAndBlocksExit() {
        auto *backend = new IntegrationSimulationBackend; PackageSimulationService simulation(backend);
        MainWindow window(nullptr, false); window.show(); QTRY_VERIFY(!window.hasBackgroundWork());
        inject(window, new PackagePreviewPage(nullptr, &simulation, nullptr), nullptr, PageId::PackagePreview);
        window.registerPackageSimulation(&simulation); simulation.startPreview();
        QVERIFY(window.hasBackgroundWork()); QVERIFY(window.m_jobStrip->isVisible());
        window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(simulation.isBusy());
        answer(QStringLiteral("OK")); QVERIFY(!window.close());
        backend->complete(); QVERIFY(!simulation.isBusy()); QVERIFY(simulation.result().success);
        QVERIFY(!window.hasBackgroundWork()); QVERIFY(window.m_jobLabel->text().contains(QStringLiteral("Preview complete")));
        QVERIFY(window.m_jobDetails->isEnabled()); QVERIFY(window.close());
    }
    void inputPreferencesUseNavigationProtectionAndRetainBackgroundWrites() {
        auto *backend = new IntegrationInputBackend; InputSettings settings(backend); QTRY_VERIFY(settings.isReady());
        MainWindow window(nullptr, false); window.show(); window.navigateTo(QStringLiteral("Hardware and Sound"));
        inject(window, new InputDevicesPage(nullptr, &settings, nullptr), &settings, PageId::InputDevices);
        QCOMPARE(window.currentSettingsSession(), static_cast<SettingsSession *>(&settings));
        settings.selectProperty(QStringLiteral("event1"), QStringLiteral("leftHanded"), true);
        QVERIFY(settings.isDirty()); answer(QStringLiteral("Cancel")); window.goBack(); QVERIFY(settings.isDirty());
        QCOMPARE(window.m_historyIndex, 1);
        answer(QStringLiteral("Discard")); window.goBack(); QVERIFY(!settings.isDirty()); QCOMPARE(window.m_historyIndex, 0);
        inject(window, new InputDevicesPage(nullptr, &settings, nullptr), &settings, PageId::InputDevices);
        settings.selectProperty(QStringLiteral("event1"), QStringLiteral("leftHanded"), true); backend->hold = true;
        settings.apply(); QVERIFY(settings.isBusy()); QVERIFY(window.hasBackgroundWork());
        window.m_scroll->takeWidget()->deleteLater();
        answer(QStringLiteral("OK")); QVERIFY(!window.close()); backend->complete();
        QVERIFY(!window.hasBackgroundWork()); QVERIFY(!settings.isDirty()); QVERIFY(window.m_jobDetails->isEnabled());
        QVERIFY(window.close());
    }
    void keyboardGuardAndJobSurvivePageDestruction() {
        auto *backend = new IntegrationKeyboardBackend; KeyboardSettings settings(backend);
        QTRY_VERIFY(settings.isReady());
        MainWindow window(nullptr, false); window.show();
        inject(window, new KeyboardPage(nullptr, &settings, nullptr), &settings, PageId::Keyboard);
        QCOMPARE(window.currentSettingsSession(), static_cast<SettingsSession *>(&settings));
        settings.selectValue("RepeatDelay", 350);
        answer("Cancel"); window.goBack(); QCOMPARE(window.m_historyIndex, 1); QCOMPARE(backend->writes, 0);
        answer("Apply"); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0);
        QCOMPARE(backend->writes, 1); QVERIFY(!settings.isDirty());
        inject(window, new KeyboardPage(nullptr, &settings, nullptr), &settings, PageId::Keyboard);
        settings.selectValue("RepeatDelay", 450); backend->hold = true; settings.apply();
        QVERIFY(settings.isBusy()); QVERIFY(window.hasBackgroundWork());
        delete window.m_scroll->takeWidget();
        answer("OK"); QVERIFY(!window.close()); backend->finish();
        QVERIFY(!window.hasBackgroundWork()); QVERIFY(!settings.isDirty());
        QVERIFY(window.m_jobLabel->text().contains("Keyboard")); QVERIFY(window.close());
    }
    void defaultProgramsGuardAndJobSurvivePageDestruction() {
        auto *backend = new IntegrationDefaultAppsBackend; DefaultAppsSettings settings(backend);
        QTRY_VERIFY(settings.isReady());
        MainWindow window(nullptr, false); window.show();
        inject(window, new DefaultAppsPage(nullptr, &settings, nullptr), &settings, PageId::DefaultApps);
        QCOMPARE(window.currentSettingsSession(), static_cast<SettingsSession *>(&settings));
        settings.selectRole(DefaultAppRole::Browser, "second.desktop");
        answer("Cancel"); window.goBack(); QCOMPARE(window.m_historyIndex, 1); QVERIFY(settings.isDirty());
        answer("Discard"); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!settings.isDirty());
        QCOMPARE(backend->writes, 0);
        inject(window, new DefaultAppsPage(nullptr, &settings, nullptr), &settings, PageId::DefaultApps);
        settings.selectRole(DefaultAppRole::Browser, "second.desktop"); backend->hold = true;
        answer("Apply"); window.goBack(); QVERIFY(settings.isBusy()); QVERIFY(window.hasBackgroundWork());
        delete window.m_scroll->takeWidget();
        answer("OK"); QVERIFY(!window.close()); backend->finish();
        QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!window.hasBackgroundWork()); QVERIFY(!settings.isDirty());
        QVERIFY(window.m_jobLabel->text().contains("Default Programs")); QVERIFY(window.close());
    }
    void shortcutsGuardAndJobSurvivePageDestruction_data() {
        QTest::addColumn<int>("shortcutScope");
        QTest::newRow("global") << int(ShortcutsPage::Scope::Global);
        QTest::newRow("standard-applications") << int(ShortcutsPage::Scope::StandardApplications);
    }
    void shortcutsGuardAndJobSurvivePageDestruction() {
        QFETCH(int, shortcutScope);
        const auto scope = static_cast<ShortcutsPage::Scope>(shortcutScope);
        const auto page = scope == ShortcutsPage::Scope::Global ? PageId::Shortcuts : PageId::StandardShortcuts;
        auto *backend = new IntegrationShortcutsBackend; ShortcutsSettings settings(backend);
        QTRY_VERIFY(settings.isReady());
        MainWindow window(nullptr, false); window.show();
        inject(window, new ShortcutsPage(nullptr, &settings, scope, nullptr), &settings, page);
        QCOMPARE(window.currentSettingsSession(), static_cast<SettingsSession *>(&settings));
        QVERIFY(settings.selectShortcuts(backend->actionId, {QKeySequence("Ctrl+Alt+F6, Ctrl+Alt+F7")}));
        answer("Cancel"); window.goBack(); QCOMPARE(window.m_historyIndex, 1); QCOMPARE(backend->writes, 0);
        answer("Discard"); window.goBack(); QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!settings.isDirty());
        inject(window, new ShortcutsPage(nullptr, &settings, scope, nullptr), &settings, page);
        QVERIFY(settings.selectShortcuts(backend->actionId, {QKeySequence("Ctrl+Alt+F6, Ctrl+Alt+F7")}));
        backend->hold = true; answer("Apply"); window.goBack();
        QVERIFY(settings.isBusy()); QVERIFY(window.hasBackgroundWork());
        delete window.m_scroll->takeWidget();
        answer("OK"); QVERIFY(!window.close()); backend->finish();
        QTRY_COMPARE(window.m_historyIndex, 0); QVERIFY(!window.hasBackgroundWork()); QVERIFY(!settings.isDirty());
        const auto title = scope == ShortcutsPage::Scope::Global ? "Keyboard Shortcuts" : "Application Shortcuts";
        QVERIFY(window.m_jobLabel->text().contains(title)); QVERIFY(window.close());
    }
    void retainedImportResumesOffPageAndCloseRequiresExplicitDiscard() {
        auto *gb = new IntegrationShortcutsBackend, *sb = new IntegrationShortcutsBackend;
        auto *cb = new IntegrationCustomShortcutsBackend;
        ShortcutsSettings globals(gb), standards(sb); CustomShortcutsSettings custom(cb);
        QTRY_VERIFY(globals.isReady() && standards.isReady() && custom.isReady());
        ShortcutImportCoordinator coordinator(&globals, &standards, &custom);
        MainWindow window(nullptr, false); window.show(); window.setShortcutImportCoordinator(&coordinator);
        const QByteArray source = "[kwin][Global Shortcuts]\nMinimize=Meta+F7\n";
        QVERIFY(coordinator.loadData(source)); QVERIFY(window.m_resumeImport->isVisible()); QVERIFY(window.m_jobStrip->isVisible());
        QVERIFY(window.m_jobDetails->isEnabled()); QVERIFY(!window.currentSettingsSession());
        window.m_resumeImport->click(); QTRY_VERIFY(window.findChild<ShortcutSchemeDialog *>());
        auto *dialog = window.findChild<ShortcutSchemeDialog *>(); QCOMPARE(dialog->importCoordinator(), &coordinator);
        dialog->reject(); QTRY_VERIFY(!QApplication::activeModalWidget());
        window.m_scroll->setWidget(new QWidget); window.m_resumeImport->click();
        QTRY_VERIFY(QApplication::activeModalWidget());
        dialog = qobject_cast<ShortcutSchemeDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        QCOMPARE(dialog->importCoordinator()->sourceBytes(), source); dialog->reject();
        QTRY_VERIFY(!QApplication::activeModalWidget());
        answer("Cancel"); QVERIFY(!window.close()); QVERIFY(coordinator.hasDocument()); QVERIFY(window.isVisible());
        answer("Discard import review"); QVERIFY(!window.close()); QTRY_VERIFY(!window.isVisible());
        QVERIFY(!coordinator.hasDocument()); QCOMPARE(gb->writes + sb->writes + cb->writes, 0);
    }
    void discardRetainedReviewPreservesStagedSettingsAndNavigationUsesInjectedSessions() {
        auto *gb = new IntegrationShortcutsBackend, *sb = new IntegrationShortcutsBackend;
        auto *cb = new IntegrationCustomShortcutsBackend;
        ShortcutsSettings globals(gb), standards(sb); CustomShortcutsSettings custom(cb);
        QTRY_VERIFY(globals.isReady() && standards.isReady() && custom.isReady());
        ShortcutImportCoordinator coordinator(&globals, &standards, &custom);
        MainWindow window(nullptr, false); window.show(); window.setShortcutImportCoordinator(&coordinator);
        QVERIFY(coordinator.loadData("[kwin][Global Shortcuts]\nMinimize=Meta+F7\n")); QVERIFY(coordinator.stageBindings());
        QVERIFY(globals.isDirty()); coordinator.discard(); QVERIFY(globals.isDirty());
        answer("Cancel"); QVERIFY(!window.close()); QVERIFY(globals.isDirty());
        answer("Discard"); QVERIFY(!window.close()); QTRY_VERIFY(!window.isVisible());
        QVERIFY(!globals.isDirty()); QCOMPARE(gb->writes + sb->writes + cb->writes, 0);
        MainWindow reopened(nullptr, false); reopened.setShortcutImportCoordinator(&coordinator);
        reopened.showEntry(PageRegistry::pathFor(PageId::Shortcuts));
        auto *page = qobject_cast<ShortcutsPage *>(reopened.m_scroll->widget()); QVERIFY(page);
        QCOMPARE(page->settingsSession(), static_cast<SettingsSession *>(&globals)); QCOMPARE(page->importCoordinator(), &coordinator);
        reopened.showEntry(PageRegistry::pathFor(PageId::CustomShortcuts));
        QCOMPARE(reopened.currentSettingsSession(), static_cast<SettingsSession *>(&custom));
    }
    void closingChecksImportedDraftsFromEveryManagedScope() {
        auto *globalBackend = new IntegrationShortcutsBackend, *standardBackend = new IntegrationShortcutsBackend;
        ShortcutsSettings globals(globalBackend), standards(standardBackend);
        QTRY_VERIFY(globals.isReady() && standards.isReady());
        MainWindow window(nullptr, false); window.show();
        window.registerSettingsSession(&globals, QStringLiteral("Keyboard Shortcuts"));
        window.registerSettingsSession(&standards, QStringLiteral("Application Shortcuts"));
        window.m_scroll->setWidget(new QWidget); QVERIFY(!window.currentSettingsSession());
        QString error;
        QVERIFY(ShortcutsSettings::stageTogether(&globals, {{globalBackend->actionId, {QKeySequence("Meta+F7")}}},
            &standards, {{standardBackend->actionId, {QKeySequence("Meta+F8")}}}, false, error));
        QVERIFY(window.m_jobStrip->isVisible()); QVERIFY(window.m_jobDetails->isEnabled());
        QVERIFY(window.m_jobLabel->text().contains("Pending changes"));
        QVERIFY(window.m_jobLabel->text().contains("Application Shortcuts"));
        answer("Cancel"); QVERIFY(!window.close()); QVERIFY(globals.isDirty() && standards.isDirty());
        globalBackend->hold = true; answer("Apply"); QVERIFY(!window.close());
        QVERIFY(globals.isBusy()); QVERIFY(window.hasBackgroundWork());
        globalBackend->finish();
        bool canceledSecond = false;
        QTimer::singleShot(0, &window, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                if (auto *cancel = box->button(QMessageBox::Cancel)) { cancel->click(); canceledSecond = true; }
            }
        });
        QTRY_VERIFY(canceledSecond); QVERIFY(!window.m_navigationGuard->pending()); QVERIFY(window.isVisible());
        QVERIFY(!globals.isDirty()); QVERIFY(standards.isDirty());
        QCOMPARE(globalBackend->writes, 1); QCOMPARE(standardBackend->writes, 0);
        answer("Discard"); QVERIFY(!window.close()); QTRY_VERIFY(!window.isVisible());
        QVERIFY(!standards.isDirty()); QCOMPARE(standardBackend->writes, 0);
    }
    void audioOperationOutlivesDialogAndRetainsOutcome() {
        auto *backend = new IntegrationAudioBackend; AudioSettings settings(backend);
        QTRY_VERIFY(settings.isReady());
        MainWindow window(nullptr, false); window.show(); window.registerAudioSettings(&settings);
        auto *dialog = new SoundDialog(&settings, &window); dialog->show();
        settings.requestChange(audioVolumeRequest(settings.snapshot(), 50));
        QVERIFY(settings.isBusy()); QVERIFY(window.hasBackgroundWork());
        delete dialog;
        QVERIFY(settings.isBusy());
        answer(QStringLiteral("OK")); QVERIFY(!window.close());
        backend->complete();
        QVERIFY(!window.hasBackgroundWork()); QVERIFY(!settings.resultText().isEmpty());
        QVERIFY(window.m_jobLabel->text().contains(QStringLiteral("Sound:")));
        QVERIFY(window.m_jobDetails->isEnabled());
        SoundDialog reopened(&settings, &window);
        QCOMPARE(reopened.findChild<QSpinBox *>(QStringLiteral("playbackVolume"))->value(), 50);
        QVERIFY(window.close());
    }
    void captureDisplayPowerIfRequested() {
        const QString output = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
        if (output.isEmpty()) QSKIP("Optional display/power visual captures");
        QVERIFY(QDir().mkpath(output)); Aero::registerStylesheet(qApp);
        auto *displayBackend = new IntegrationDisplayBackend;
        displayBackend->current = displayMultiFixture();
        DisplaySettings display(displayBackend);
        auto *powerBackend=new IntegrationPowerBackend;
        auto &unknown=powerBackend->current.profiles[0].keys[PowerPolicyKey::LockBeforeDisplayOff];
        unknown.valueKnown=false; unknown.localPresent=unknown.effectivePresent=false;
        PowerPolicySettings power(powerBackend);
        PowerProfileService profiles(std::make_unique<IntegrationPowerProfiles>());
        QTRY_VERIFY(!display.isBusy() && !power.isBusy());
        MainWindow window(nullptr, false); window.resize(1060, 840); window.show();
        auto capture = [&](const QString &name) {
            QTest::qWait(250); return window.grab().save(output + '/' + name + ".png");
        };
        inject(window, new DisplayPage(window.buildSubpageSidebar({}, {}), &display, nullptr), &display, PageId::Display);
        display.selectOutput(2); display.selectScale(1.25); display.selectRotation(2);
        display.selectPosition(1920, 100);
        QVERIFY(capture("display-layout"));
        display.apply(); QVERIFY(display.awaitingConfirmation());
        window.m_scroll->verticalScrollBar()->setValue(window.m_scroll->verticalScrollBar()->maximum());
        QVERIFY(capture("display-layout-confirmation")); display.revert();
        inject(window, new PowerOptionsPage(window.buildSubpageSidebar(PowerOptionsPage::sidebarLinks(), PowerOptionsPage::sidebarSeeAlso()), &power, &profiles, nullptr), &power, PageId::PowerOptions);
        power.selectIdleAction("AC", 2); power.selectPowerButtonAction("AC", 32);
        power.selectDimTimeout("AC", 60); power.selectBatteryLevels(20, 10);
        QVERIFY(capture("power-policy-overview"));
        auto *advanced = window.findChild<QPushButton *>("powerAdvancedToggle");
        QVERIFY(advanced); advanced->setChecked(true); QTest::qWait(100);
        window.m_scroll->verticalScrollBar()->setValue(window.m_scroll->verticalScrollBar()->maximum() / 2);
        QVERIFY(capture("power-policy-actions"));
        window.m_scroll->verticalScrollBar()->setValue(window.m_scroll->verticalScrollBar()->maximum());
        QVERIFY(capture("power-policy-battery"));
        power.revert();
    }
    void captureScreenLockIfRequested() {
        const auto output = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
        if (output.isEmpty()) QSKIP("Optional Screen Locking Aero captures");
        QVERIFY(QDir().mkpath(output)); Aero::registerStylesheet(qApp);
        auto *backend = new IntegrationScreenLockBackend; ScreenLockSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); MainWindow window(nullptr, false); window.resize(1120, 980); window.show();
        ScreenLockAppearance::Evidence appearance; appearance.approvedIdentity = appearance.providerVerified = appearance.backgroundAvailable = true;
        appearance.shellId = QStringLiteral("io.gitgud.wackyideas.desktop"); appearance.loginTheme = QStringLiteral("sddm-theme-mod");
        appearance.description = QStringLiteral("Aero background file: /fixture/aero/background. Passive file evidence; active lock-screen rendering is unverified.");
        appearance.reason = QStringLiteral("The fixed Aero background shares privileged SDDM storage. A separate authorized, atomic asset transaction is required.");
        inject(window, new ScreenLockPage(window.buildSubpageSidebar(ScreenLockPage::sidebarLinks(), ScreenLockPage::sidebarSeeAlso()), &session, appearance, nullptr), &session, PageId::ScreenLock);
        auto capture = [&](const QString &name, bool bottom=false) {
            QTest::qWait(30); window.m_scroll->verticalScrollBar()->setValue(bottom ? window.m_scroll->verticalScrollBar()->maximum() : 0);
            QTest::qWait(180); return window.grab().save(output + '/' + name + ".png");
        };
        QVERIFY(capture("screen-lock-policy")); session.selectTimeout(15.0); session.selectGrace(60);
        QVERIFY(capture("screen-lock-draft", true)); session.selectNoPassword(); QVERIFY(capture("screen-lock-passwordless")); session.revert();
        backend->current.fields[ScreenLockKey::Timeout].value = 0.5; backend->current.fields[ScreenLockKey::LockGrace].value = -5;
        session.revert(); QVERIFY(capture("screen-lock-native-legacy"));
        backend->current = screenLockFixture(); session.revert(); backend->notified = false;
        session.selectTimeout(10.0); session.apply(); QTRY_VERIFY(!session.isBusy()); QVERIFY(session.hasConflict());
        QVERIFY(capture("screen-lock-uncertain", true)); session.revert();
        backend->notified = true; backend->hold = true; session.selectTimeout(15.0); session.apply();
        QVERIFY(session.isBusy()); QVERIFY(session.isDirty());
        emit backend->operationPending(backend->pending, QStringLiteral("Saving is taking longer than expected. The operation continues; wait for it to finish before retrying or closing Control Panel."));
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("screenLockMessage"))->text().contains(QStringLiteral("taking longer")));
        QVERIFY(capture("screen-lock-saving-continues", true)); backend->finish(); QTRY_VERIFY(!session.isBusy());
    }
    void captureAeroLoginIfRequested(){
        const auto output=qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");if(output.isEmpty())QSKIP("Optional Aero login and lock appearance captures");QVERIFY(QDir().mkpath(output));Aero::registerStylesheet(qApp);
        auto *backend=new IntegrationAeroLoginBackend;AeroLoginSettings session(backend);QTRY_VERIFY(!session.isBusy());MainWindow window(nullptr,false);window.resize(1120,1080);window.show();
        inject(window,new AeroLoginPage(window.buildSubpageSidebar(AeroLoginPage::sidebarLinks(),AeroLoginPage::sidebarSeeAlso()),&session,nullptr),&session,PageId::AeroLogin);
        auto capture=[&](const QString &name,bool bottom=false){QTest::qWait(20);window.m_scroll->verticalScrollBar()->setValue(bottom?window.m_scroll->verticalScrollBar()->maximum():0);QTest::qWait(120);return window.grab().save(output+'/'+name+".png");};
        QVERIFY(capture("aero-login-options"));session.removeImage();QVERIFY(capture("aero-login-blue-and-lock-fallback",true));
        bool captured=false;QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QDialog *>("aeroLoginApplyReviewDialog");QVERIFY(dialog);captured=dialog->grab().save(output+"/aero-login-apply-review.png");dialog->reject();});window.findChild<QPushButton *>("aeroLoginApply")->click();QVERIFY(captured);QCOMPARE(backend->writes,0);
        session.apply();QVERIFY(!session.isDirty());QVERIFY(capture("aero-login-saved-files",true));session.revert();
        backend->current.recoveryPending=backend->current.canRecover=true;backend->current.writable=false;
        backend->current.fields[AeroLoginKey::StartupSound].value=false;backend->current.fields[AeroLoginKey::StartupSound].localPresent=true;backend->current.fields[AeroLoginKey::StartupSound].raw=QStringLiteral("false");
        backend->current.recoverySummary=QStringLiteral("Roll back the reviewed interrupted transaction.\nStartup sound: disabled → enabled (saved override absent).\nOriginal picture: present, 405 bytes, SHA-256 %1.\nRecovery restores saved bytes; greeter and lock adoption remain unverified.").arg(QString::fromLatin1(backend->current.approvedDefault.digest.toHex()));
        session.refresh();captured=false;QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QDialog *>("aeroLoginRecoveryReviewDialog");QVERIFY(dialog);QVERIFY(dialog->findChild<QLabel *>("aeroLoginReviewText")->text().contains("disabled → enabled"));captured=dialog->grab().save(output+"/aero-login-recovery-review.png");dialog->reject();});window.findChild<QPushButton *>("aeroLoginRecover")->click();QVERIFY(captured);QCOMPARE(backend->recoveries,0);
        backend->current.recoveryPending=backend->current.canRecover=false;
        backend->current.trustedInstallation=backend->current.writable=backend->current.helperAvailable=false;backend->current.reason="The installed Aero assets are owned by nobody. Administrative installation is required before saving.";backend->current.helperReason="The approved root-owned helper, policy and identity manifest are unavailable.";session.refresh();QVERIFY(capture("aero-login-installation-readonly",true));
    }
    void capturePendingSavesIfRequested() {
        const auto output = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
        if (output.isEmpty()) QSKIP("Optional pending save Aero captures");
        QVERIFY(QDir().mkpath(output)); Aero::registerStylesheet(qApp);
        {
            auto *backend = new IntegrationNightTimeBackend; backend->hold = true; NightTimeSettings session(backend); QTRY_VERIFY(!session.isBusy());
            MainWindow window(nullptr, false); window.resize(1120, 980); window.show();
            inject(window, new NightTimePage(window.buildSubpageSidebar(NightTimePage::sidebarLinks(), NightTimePage::sidebarSeeAlso()), &session, new NightTimeLocation(NightTimeLocation::Factory{}), nullptr), &session, PageId::NightTime);
            session.selectValue(NightTimeKey::Source, 1); session.apply(); QVERIFY(session.isBusy()); QVERIFY(session.isDirty());
            emit backend->operationPending(backend->pending, QStringLiteral("Saving Day and Night is taking longer than expected and continues in the background. Wait for it to finish before retrying or closing Control Panel."));
            QVERIFY(window.findChild<QLabel *>(QStringLiteral("nightTimeMessage"))->text().contains(QStringLiteral("continues")));
            QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("nightTimeApply"))->isEnabled());
            QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("nightTimeRevert"))->isEnabled());
            QTest::qWait(50); window.m_scroll->verticalScrollBar()->setValue(window.m_scroll->verticalScrollBar()->maximum()); QTest::qWait(150);
            QVERIFY(window.grab().save(output + QStringLiteral("/day-night-saving-continues.png"))); backend->finish(); QTRY_VERIFY(!session.isBusy()); QVERIFY(!session.isDirty());
        }
        {
            auto *backend = new IntegrationNightLightBackend; backend->hold = true; NightLightSettings session(backend); QTRY_VERIFY(!session.isBusy());
            MainWindow window(nullptr, false); window.resize(1120, 980); window.show();
            inject(window, new NightLightPage(window.buildSubpageSidebar(NightLightPage::sidebarLinks(), NightLightPage::sidebarSeeAlso()), &session, nullptr), &session, PageId::NightLight);
            session.selectValue(NightLightKey::NightTemperature, 3900); session.apply(); QVERIFY(session.isBusy()); QVERIFY(session.isDirty());
            emit backend->operationPending(backend->pending, QStringLiteral("Saving Night Light is taking longer than expected and continues in the background. Wait for it to finish before retrying or closing Control Panel."));
            QVERIFY(window.findChild<QLabel *>(QStringLiteral("nightLightMessage"))->text().contains(QStringLiteral("continues")));
            QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("nightLightApply"))->isEnabled());
            QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("nightLightRevert"))->isEnabled());
            QTest::qWait(50); window.m_scroll->verticalScrollBar()->setValue(window.m_scroll->verticalScrollBar()->maximum()); QTest::qWait(150);
            QVERIFY(window.grab().save(output + QStringLiteral("/night-light-saving-continues.png"))); backend->finish(); QTRY_VERIFY(!session.isBusy()); QVERIFY(!session.isDirty());
        }
    }
    void captureNightTimeIfRequested() {
        const auto output = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
        if (output.isEmpty()) QSKIP("Optional Day and Night Aero captures");
        QVERIFY(QDir().mkpath(output)); Aero::registerStylesheet(qApp);
        auto *backend = new IntegrationNightTimeBackend; NightTimeSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); MainWindow window(nullptr, false); window.resize(1120, 980); window.show();
        inject(window, new NightTimePage(window.buildSubpageSidebar(NightTimePage::sidebarLinks(), NightTimePage::sidebarSeeAlso()), &session, new NightTimeLocation(NightTimeLocation::Factory{}), nullptr), &session, PageId::NightTime);
        auto capture = [&](const QString &name, bool bottom=false) {
            QTest::qWait(30); window.m_scroll->verticalScrollBar()->setValue(bottom ? window.m_scroll->verticalScrollBar()->maximum() : 0);
            QTest::qWait(180); return window.grab().save(output + '/' + name + ".png");
        };
        QVERIFY(capture("day-night-automatic"));
        session.selectValue(NightTimeKey::Source, 1); session.selectTime(NightTimeKey::SunriseStart, QTime(7,15));
        QVERIFY(capture("day-night-custom-times")); session.selectDuration(7200); QVERIFY(capture("day-night-forecast", true));
        session.revert(); session.selectManualLocation(85.0, 20.0); QVERIFY(capture("day-night-manual-map"));
        auto &f = backend->current.fields[NightTimeKey::Automatic]; f.localPresent = false; f.globalPresent = true; f.value = false;
        backend->current.cachedLocation = {true, 51.5, 0.0, {}}; backend->current.revision = "dual-view-capture";
        session.revert(); window.findChild<QPushButton *>(QStringLiteral("nightTimeDetailsToggle"))->setChecked(true); QVERIFY(capture("day-night-dual-view"));
        window.findChild<QPushButton *>(QStringLiteral("nightTimeDetailsToggle"))->setChecked(false);
        backend->current = nightTimeFixture(); backend->notified = false; session.revert();
        session.selectValue(NightTimeKey::Source, 1); session.apply(); QTRY_VERIFY(!session.isBusy());
        QVERIFY(session.hasConflict()); QVERIFY(capture("day-night-uncertain-apply", true)); session.revert();
    }
    void captureNightLightIfRequested() {
        const auto output = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
        if (output.isEmpty()) QSKIP("Optional Night Light Aero captures");
        QVERIFY(QDir().mkpath(output)); Aero::registerStylesheet(qApp);
        auto *backend = new IntegrationNightLightBackend; NightLightSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); MainWindow window(nullptr, false); window.resize(1120, 980); window.show();
        inject(window, new NightLightPage(window.buildSubpageSidebar(NightLightPage::sidebarLinks(), NightLightPage::sidebarSeeAlso()), &session, nullptr), &session, PageId::NightLight);
        auto capture = [&](const QString &name, bool bottom=false) {
            window.m_scroll->verticalScrollBar()->setValue(bottom ? window.m_scroll->verticalScrollBar()->maximum() : 0);
            QTest::qWait(180); return window.grab().save(output + '/' + name + ".png");
        };
        session.selectValue(NightLightKey::NightTemperature, 3800); QVERIFY(capture("night-light-scheduled"));
        session.selectValue(NightLightKey::Mode, 0); QVERIFY(capture("night-light-constant")); session.revert();
        auto &day = backend->current.fields[NightLightKey::DayTemperature]; day.value = 20000; day.defaultNoOp = true;
        backend->current.fields[NightLightKey::Mode].value = 17; backend->current.runtime.inhibited = true; backend->current.runtime.running = false;
        backend->current.runtime.previousTransitionDateTime = 1700000000; backend->current.runtime.previousTransitionDuration = 1800000;
        backend->current.runtime.scheduledTransitionDateTime = 1700043200; backend->current.runtime.scheduledTransitionDuration = 1800000;
        backend->current.revision = "capture-foreign"; session.revert();
        QVERIFY(capture("night-light-unknown-values")); QVERIFY(capture("night-light-runtime", true));
        backend->current = nightLightFixture(); backend->notified = false; session.revert();
        session.selectValue(NightLightKey::NightTemperature, 3900); session.apply(); QTRY_VERIFY(!session.isBusy());
        QVERIFY(session.isDirty()); QVERIFY(session.hasConflict()); QVERIFY(capture("night-light-uncertain-apply", true));
        session.revert();
    }
    void captureBatch18DialogsIfRequested() {
        const QString output=qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
        if(output.isEmpty()) QSKIP("Optional Batch18 dialog captures");
        QVERIFY(QDir().mkpath(output)); Aero::registerStylesheet(qApp);
        DisplaySettings display(new IntegrationDisplayBackend);
        auto *powerBackend=new IntegrationPowerBackend;
        auto &defaultState=powerBackend->current.profiles[0].keys[PowerPolicyKey::DisplaySeconds];
        defaultState.defaultValue=300; defaultState.resettable=true; defaultState.resetReason.clear();
        PowerPolicySettings power(powerBackend);
        PowerProfileService profiles(std::make_unique<IntegrationPowerProfiles>());
        QTRY_VERIFY(!display.isBusy()&&!power.isBusy());
        MainWindow window(nullptr,false); window.resize(1060,840); window.show();
        bool captured=false;
        auto armCapture=[&](const QString &dialogName,const QString &fileName,const std::function<void(QDialog *)> &configure) {
            captured=false;
            QTimer::singleShot(0,&window,[&,dialogName,fileName,configure] {
                auto *dialog=window.findChild<QDialog *>(dialogName);
                if(!dialog) { if(auto *modal=qobject_cast<QDialog *>(QApplication::activeModalWidget())) modal->reject(); return; }
                if(configure) configure(dialog);
                QTest::qWait(150); captured=dialog->grab().save(output+'/'+fileName+".png"); dialog->reject();
            });
        };
        inject(window,new DisplayPage(window.buildSubpageSidebar({},{}),&display,nullptr),&display,PageId::Display);
        auto *scale=window.findChild<QComboBox *>("displayScale"); QVERIFY(scale);
        scale->setCurrentIndex(scale->findData(QStringLiteral("custom"),Qt::UserRole+1));
        armCapture("displayCustomScaleDialog","display-custom-scale",[](QDialog *dialog) {
            if(auto *percent=dialog->findChild<QDoubleSpinBox *>("displayCustomScalePercent")) percent->setValue(137);
        });
        QTRY_VERIFY(captured); QVERIFY(!display.isDirty());
        inject(window,new PowerOptionsPage(window.buildSubpageSidebar(PowerOptionsPage::sidebarLinks(),PowerOptionsPage::sidebarSeeAlso()),&power,&profiles,nullptr),&power,PageId::PowerOptions);
        auto *timeout=window.findChild<QComboBox *>("powerDisplayTimeout_AC"); QVERIFY(timeout);
        int custom=-1; for(int i=0;i<timeout->count();++i) if(timeout->itemText(i).contains("Custom seconds")) custom=i;
        QVERIFY(custom>=0);
        timeout->setCurrentIndex(custom); QVERIFY(QMetaObject::invokeMethod(timeout,"activated",Q_ARG(int,custom)));
        armCapture("powerCustomTimeoutDialog","power-custom-timeout",[](QDialog *dialog) {
            if(auto *seconds=dialog->findChild<QSpinBox *>("powerCustomTimeoutSeconds")) seconds->setValue(1234);
        });
        QTRY_VERIFY(captured); QVERIFY(!power.isDirty());
        armCapture("powerDefaultsDialog","power-defaults-review",{});
        auto *defaults=window.findChild<QPushButton *>("powerPolicyDefaults"); QVERIFY(defaults); defaults->click();
        QTRY_VERIFY(captured); QVERIFY(!power.isDirty());
    }
    void captureWindowsStyleIfRequested() {
        const QString output = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
        if (output.isEmpty()) QSKIP("Optional visual captures");
        QVERIFY(QDir().mkpath(output)); Aero::registerStylesheet(qApp);
        DisplaySettings display(new IntegrationDisplayBackend);
        AppearanceSettings appearance(new IntegrationAppearanceBackend);
        PowerPolicySettings power(new IntegrationPowerBackend);
        PowerProfileService profiles(std::make_unique<IntegrationPowerProfiles>());
        QTRY_VERIFY(!display.isBusy() && !appearance.isBusy() && !power.isBusy());
        MainWindow window(nullptr, false); window.show();
        QTRY_VERIFY(!window.hasBackgroundWork());
        auto capture = [&](const QString &name) { QTest::qWait(2200); return window.grab().save(output + '/' + name + ".png"); };
        inject(window, new DisplayPage(window.buildSubpageSidebar({}, {}), &display, nullptr), &display, PageId::Display);
        display.selectMode(QStringLiteral("2")); QVERIFY(capture(QStringLiteral("screen-resolution")));
        display.apply(); QVERIFY(capture(QStringLiteral("display-confirmation"))); display.revert();
        inject(window, new WindowBehaviorPage(window.buildSubpageSidebar(WindowBehaviorPage::sidebarLinks(), WindowBehaviorPage::sidebarSeeAlso()), &appearance, nullptr), &appearance, PageId::WindowBehavior);
        appearance.selectAction(QStringLiteral("Minimize")); QVERIFY(capture(QStringLiteral("window-behavior")));
        inject(window, new PowerOptionsPage(window.buildSubpageSidebar(PowerOptionsPage::sidebarLinks(), PowerOptionsPage::sidebarSeeAlso()), &power, &profiles, nullptr), &power, PageId::PowerOptions);
        power.selectDisplayTimeout(QStringLiteral("AC"), 300); QVERIFY(capture(QStringLiteral("power-policies")));

        WallpaperSettings wallpaper(new IntegrationWallpaperBackend);
        auto *themeBackend = new FixtureAeroThemeBackend;
        themeBackend->current.values["iconTheme"] = QStringLiteral("breeze");
        AeroThemeSettings components(themeBackend);
        FontSettings fonts(new IntegrationFontBackend);
        QTRY_VERIFY(!wallpaper.isBusy() && !components.isBusy() && !fonts.isBusy());
        QImage picture(640, 360, QImage::Format_RGB32);
        for (int y = 0; y < picture.height(); ++y) for (int x = 0; x < picture.width(); ++x)
            picture.setPixelColor(x, y, QColor(25 + x / 4, 75 + y / 3, 180));
        const QString picturePath = environment.filePath("background.png"); QVERIFY(picture.save(picturePath));
        wallpaper.selectPicture(picturePath);
        inject(window, new DesktopBackgroundPage(window.buildSubpageSidebar(DesktopBackgroundPage::sidebarLinks(), DesktopBackgroundPage::sidebarSeeAlso()), &wallpaper, nullptr), &wallpaper, PageId::DesktopBackground);
        QVERIFY(capture(QStringLiteral("desktop-background")));
        inject(window, new ThemeComponentsPage(window.buildSubpageSidebar(ThemeComponentsPage::sidebarLinks(), ThemeComponentsPage::sidebarSeeAlso()), &components, nullptr), &components, PageId::ThemeComponents);
        components.selectValue(QStringLiteral("aeroIntensity"), 60); QVERIFY(capture(QStringLiteral("appearance-components")));
        inject(window, new PersonalizationPage(window.buildSubpageSidebar(PersonalizationPage::sidebarLinks(), PersonalizationPage::sidebarSeeAlso()), &components, nullptr), &components, PageId::Personalization);
        QVERIFY(capture(QStringLiteral("aero-personalization")));
        components.stageRestoreDefaults();
        window.m_scroll->verticalScrollBar()->setValue(window.m_scroll->verticalScrollBar()->maximum());
        QVERIFY(capture(QStringLiteral("restore-aero-defaults")));
        inject(window, new FontSettingsPage(window.buildSubpageSidebar(FontSettingsPage::sidebarLinks(), FontSettingsPage::sidebarSeeAlso()), &fonts, nullptr), &fonts, PageId::FontSettings);
        fonts.selectFont(0, QFont(QStringLiteral("Noto Sans"), 11)); QVERIFY(capture(QStringLiteral("font-settings")));
        const QString listPath = environment.filePath("fake-fc-list"); QFile list(listPath); QVERIFY(list.open(QIODevice::WriteOnly));
        list.write("#!/bin/sh\nprintf 'DejaVu Sans\\t/tmp/fixture.ttf\\nDejaVu Sans Mono\\t/tmp/fixture-mono.ttf\\nNoto Sans\\t/tmp/fixture-noto.ttf\\n'\n"); list.close();
        QVERIFY(list.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        UserFontService::Options options; options.dataDirectory = environment.path(); options.listProgram = listPath;
        UserFontService service(options); service.refresh(); QTRY_VERIFY(!service.busy());
        inject(window, new FontsPage(window.buildSubpageSidebar(FontsPage::sidebarLinks(), FontsPage::sidebarSeeAlso()), &service), nullptr, PageId::Fonts);
        window.registerFontService(&service); QVERIFY(capture(QStringLiteral("font-browser")));
        AccountSettings account(new IntegrationAccountBackend); AeroEffectSettings aero(new IntegrationAeroBackend);
        QTRY_VERIFY(!account.isBusy() && !aero.isBusy());
        inject(window, new UserAccountsPage(window.buildSubpageSidebar(UserAccountsPage::sidebarLinks(), UserAccountsPage::sidebarSeeAlso()), &account, nullptr), &account, PageId::UserAccounts);
        account.selectName(QStringLiteral("Windows Seven User")); QVERIFY(capture(QStringLiteral("user-account-name")));
        inject(window, new AeroEffectsPage(window.buildSubpageSidebar(AeroEffectsPage::sidebarLinks(), AeroEffectsPage::sidebarSeeAlso()), &aero, nullptr), &aero, PageId::AeroEffects);
        aero.selectStrength(6); QVERIFY(capture(QStringLiteral("aero-effects")));
        auto *simulationBackend = new IntegrationSimulationBackend; PackageSimulationService simulation(simulationBackend);
        simulation.startPreview(); simulationBackend->complete(); QVERIFY(simulation.result().success);
        inject(window, new PackagePreviewPage(window.buildSubpageSidebar(PackagePreviewPage::sidebarLinks(), PackagePreviewPage::sidebarSeeAlso()), &simulation, nullptr), nullptr, PageId::PackagePreview);
        window.registerPackageSimulation(&simulation); QVERIFY(capture(QStringLiteral("package-preview")));
    }
};
QTEST_MAIN(KdeIntegrationTest)
#include "kde_integration_test.moc"
