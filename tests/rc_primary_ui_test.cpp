#include <QtTest>
#include <AeroQt/stylesheet.h>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QHBoxLayout>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <memory>
#include <functional>
#include "audio_fixture.h"
#include "aero_theme_fixture.h"
#include "night_light_fixture.h"
#include "power_policy_fixture.h"
#include "settings/InputSettings.h"
#include "settings/WallpaperSettings.h"
#include "settings/PowerProfileService.h"
#include "ui/dialogs/SoundDialog.h"
#include "ui/pages/InputDevicesPage.h"
#include "ui/pages/DesktopBackgroundPage.h"
#include "ui/pages/NightLightPage.h"
#include "ui/pages/PowerOptionsPage.h"
#include "ui/pages/PersonalizationPage.h"

namespace {
const QString failure=QStringLiteral("Fixture save denied. Your pending choices are retained. Revert to reload the current values before trying again; no live desktop provider was contacted.");

// Production page scaffolds sit in a scroll viewport below a 34px shell bar.
// This fixture preserves that available space and sidebar width without ever
// constructing MainWindow's live provider singletons or launcher services.
class LayoutShell final : public QMainWindow {
public:
    QScrollArea *viewport;
    explicit LayoutShell(const QSize &size) {
        setWindowTitle(QStringLiteral("Control Panel — isolated RC layout fixture"));
        auto *body=new QWidget; auto *layout=new QVBoxLayout(body); layout->setContentsMargins(0,0,0,0); layout->setSpacing(0);
        auto *bar=new QWidget; bar->setFixedHeight(34); auto *row=new QHBoxLayout(bar); row->setContentsMargins(8,3,6,3);
        row->addWidget(new QLabel(QStringLiteral("Control Panel  >  Primary settings")),1);
        auto *search=new QLineEdit; search->setPlaceholderText(QStringLiteral("Search Control Panel")); search->setFixedWidth(190); row->addWidget(search);
        layout->addWidget(bar); viewport=new QScrollArea; viewport->setWidgetResizable(true); viewport->setFrameShape(QFrame::NoFrame); layout->addWidget(viewport,1);
        setCentralWidget(body); resize(size);
        connect(qApp,&QApplication::focusChanged,this,[this](QWidget *,QWidget *now) {
            if (now && viewport->widget() && viewport->widget()->isAncestorOf(now)) viewport->ensureWidgetVisible(now,8,8);
        });
    }
    static QScrollArea *sidebar() {
        auto *area=new QScrollArea; area->setFixedWidth(190); area->setFrameShape(QFrame::NoFrame); area->setWidgetResizable(true);
        auto *body=new QWidget; auto *layout=new QVBoxLayout(body); layout->setContentsMargins(14,14,10,14);
        auto *home=new QPushButton(QStringLiteral("Control Panel Home")); home->setFlat(true); layout->addWidget(home); layout->addStretch(); area->setWidget(body); return area;
    }
};
void capture(QWidget &window,const QString &name) {
    const auto root=qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR"); if (root.isEmpty()) return;
    QVERIFY(QDir().mkpath(root)); QCoreApplication::processEvents();
    QVERIFY(window.grab().save(root+QLatin1Char('/')+name+QStringLiteral(".png")));
}
void reachable(LayoutShell &shell,QWidget *control) {
    QVERIFY(control); QVERIFY(control->isVisibleTo(&shell));
    // Staging or showing an error changes nested label/layout size hints. Let
    // those requests update the scroll range before asking to reveal a control;
    // the first ensure against the old range otherwise clamps prematurely.
    QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest); QCoreApplication::processEvents();
    auto reveal=[&] {
        shell.viewport->ensureWidgetVisible(control,8,8);
        return QRect(QPoint(0,0),shell.viewport->viewport()->size()).contains(
            QRect(control->mapTo(shell.viewport->viewport(),QPoint(0,0)),control->size()));
    };
    QTRY_VERIFY_WITH_TIMEOUT(reveal(),1000);
    const QRect visible(QPoint(0,0),shell.viewport->viewport()->size());
    const QRect bounds(control->mapTo(shell.viewport->viewport(),QPoint(0,0)),control->size());
    QVERIFY2(visible.contains(bounds),qPrintable(QStringLiteral("%1 is clipped after scrolling: control %2,%3 %4x%5; viewport %6x%7")
        .arg(control->objectName()).arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height()).arg(visible.width()).arg(visible.height())));
}
void tabMoves(QWidget &start,QWidget &window) {
    start.setFocus(Qt::TabFocusReason); QCoreApplication::processEvents();
    QCOMPARE(QApplication::focusWidget(),&start); QTest::keyClick(&start,Qt::Key_Tab);
    QVERIFY(QApplication::focusWidget()); QVERIFY(QApplication::focusWidget()!=&start);
    QVERIFY(QApplication::focusWidget()->window()==&window);
}
void checkPage(LayoutShell &shell,QWidget *page,SettingsSession &session,const char *applyName,const char *revertName,
               const char *messageName,const QString &name,const std::function<void()> &stage,const std::function<void()> &fail) {
    shell.viewport->setWidget(page); shell.show(); shell.activateWindow(); QCoreApplication::processEvents();
    capture(shell,name+QStringLiteral("-overview"));
    auto *apply=page->findChild<QPushButton *>(QString::fromLatin1(applyName));
    auto *revert=page->findChild<QPushButton *>(QString::fromLatin1(revertName));
    QVERIFY(apply); QVERIFY(revert); QVERIFY(!apply->isEnabled());
    stage(); QVERIFY(session.isDirty()); QVERIFY(apply->isEnabled()); reachable(shell,apply); reachable(shell,revert);
    tabMoves(*apply,shell); reachable(shell,apply); capture(shell,name+QStringLiteral("-staged-actions"));
    apply->click(); QVERIFY(session.isBusy()); QVERIFY(!apply->isEnabled());
    fail(); QTRY_VERIFY(!session.isBusy()); QVERIFY(session.isDirty()); QVERIFY(!session.errorText().isEmpty());
    auto *message=page->findChild<QLabel *>(QString::fromLatin1(messageName)); QVERIFY(message);
    QVERIFY2(message->text().contains(QStringLiteral("Fixture save denied")),qPrintable(message->text()));
    QVERIFY(message->wordWrap()); QVERIFY(message->height()>0); QVERIFY(message->width()>0);
    reachable(shell,message); capture(shell,name+QStringLiteral("-error"));
    reachable(shell,revert); QVERIFY(revert->isEnabled()); tabMoves(*revert,shell);
}
class InputFixture final : public InputBackend {
public:
    InputSnapshot current; quint64 pending=0; int writes=0;
    InputFixture() {
        current.available=current.writable=true; current.owner=QStringLiteral(":fixture-input"); current.version=QStringLiteral("6.6.6");
        current.devices={{"event1","Fixture mouse and supported scrolling","/org/kde/KWin/InputDevice/event1",{
            {"name","Fixture mouse and supported scrolling"},{"sysName","event1"},{"vendor",1u},{"product",2u},
            {"pointer",true},{"touchpad",false},{"isVirtual",false},
            {"leftHanded",false},{"supportsLeftHanded",true},{"leftHandedEnabledByDefault",false},
            {"pointerAcceleration",0.0},{"supportsPointerAcceleration",true},{"defaultPointerAcceleration",0.0},
            {"scrollFactor",1.0},{"naturalScroll",false},{"supportsNaturalScroll",true},{"naturalScrollEnabledByDefault",false},
            {"__writable",QStringList{"leftHanded","pointerAcceleration","scrollFactor","naturalScroll"}}
        }}};
    }
    void readCurrent(quint64 token) override { emit readFinished(token,true,current,{}); }
    void writeChanges(quint64 token,const InputSnapshot &,const InputSnapshot &) override { ++writes; pending=token; }
    void fail() { emit writeFinished(pending,InputOutcome::Failed,current,failure); }
};
class BackgroundFixture final : public WallpaperBackend {
public:
    WallpaperState current; int writes=0;
    BackgroundFixture() { current.desktops={{1,0,QStringLiteral("org.kde.image"),{},2,1,true,{}}}; }
    void readCurrent() override { emit readFinished(true,current,{}); }
    void writePicture(const WallpaperDesktop &,const QString &,int) override { ++writes; }
    void fail() { emit writeFinished(false,failure); }
};
class LightFixture final : public NightLightBackend {
public:
    NightLightSnapshot current=nightLightFixture(); quint64 pending=0; int writes=0;
    void readCurrent(quint64 token) override { emit readFinished(token,true,current,{}); }
    void writeChanges(quint64 token,const NightLightSnapshot &,const NightLightSnapshot &) override { ++writes; pending=token; }
    void preview(quint64 token,int,const QString &,quint64) override { emit previewFinished(token,true,false,{}); }
    void stopPreview(quint64 token,const QString &,quint64) override { emit previewFinished(token,true,true,{}); }
    void fail() { emit writeFinished(pending,false,false,failure); }
};
class PolicyFixture final : public PowerPolicyBackend {
public:
    PowerPolicySnapshot current; int writes=0;
    PolicyFixture() {
        current.serviceAvailable=true; current.owner=QStringLiteral(":fixture-power"); current.capabilityRevision=QStringLiteral("fixture-capabilities");
        current.batteryLevelsEditable=current.batteryCriticalActionEditable=true;
        current.batteryCriticalActions={{0,QStringLiteral("Do nothing")},{2,QStringLiteral("Hibernate")},{8,QStringLiteral("Shut down")}};
        for (const auto &id:{QStringLiteral("AC"),QStringLiteral("Battery"),QStringLiteral("LowBattery")}) {
            PowerPolicyProfile profile; profile.id=id; profile.displayEditable=profile.sleepEditable=true; profile.suspendAction=1;
            profile.idleActions={{0,QStringLiteral("Do nothing")},{1,QStringLiteral("Sleep")},{2,QStringLiteral("Hibernate")},{8,QStringLiteral("Shut down")}};
            profile.powerButtonActions=profile.lidActions=profile.idleActions; profile.sleepModes={{1,QStringLiteral("Standby")},{2,QStringLiteral("Hybrid sleep")}};
            profile.dimEditable=profile.brightnessEditable=profile.keyboardBrightnessEditable=true;
            profile.lockedDisplayEditable=profile.lockBeforeDisplayOffEditable=true;
            profile.powerButtonEditable=profile.powerDownEditable=profile.lidEditable=true;
            profile.lidExternalMonitorEditable=profile.sleepModeEditable=true; current.profiles<<profile;
        }
        seedPowerPolicyMetadata(current);
    }
    void readCurrent() override { emit readFinished(true,current,{}); }
    void writePolicy(const PowerPolicySnapshot &,const PowerPolicySnapshot &) override { ++writes; }
    void cancel() override {}
    void fail() { emit writeFinished(false,false,false,failure); }
};
class ProfilesFixture final : public PowerProfileBackend {
public:
    QStringList availableProfiles(QString *) override { return {QStringLiteral("balanced")}; }
    QString activeProfile(QString *) override { return QStringLiteral("balanced"); }
    void setActiveProfile(const QString &,Completion completion) override { completion(QStringLiteral("Fixture profile actions are unused by layout checks.")); }
};
class ThemeFixture final : public FixtureAeroThemeBackend {
public:
    void writeSettings(const QVariantMap &,const QVariantMap &) override { ++writes; }
    void fail() { emit writeFinished(false,failure); }
};
}

class RcPrimaryUiTest final : public QObject {
    Q_OBJECT
private slots:
    void primaryPages_data() {
        QTest::addColumn<QString>("page"); QTest::addColumn<QSize>("size");
        for (const auto &page:{QStringLiteral("input"),QStringLiteral("background"),QStringLiteral("night-light"),QStringLiteral("power"),QStringLiteral("aero")})
            for (const auto &size:{QSize(1000,650),QSize(1920,1080)}) {
                const auto name=page+QStringLiteral("-%1x%2").arg(size.width()).arg(size.height());
                QTest::newRow(name.toUtf8().constData())<<page<<size;
            }
    }
    void primaryPages() {
        QFETCH(QString,page); QFETCH(QSize,size); const auto name=QStringLiteral("rc-primary-")+QString::fromLatin1(QTest::currentDataTag());
        if (page==QLatin1String("input")) {
            auto *backend=new InputFixture; InputSettings session(backend); QTRY_VERIFY(session.isReady()); LayoutShell shell(size);
            checkPage(shell,new InputDevicesPage(LayoutShell::sidebar(),&session,nullptr),session,"inputApply","inputRevert","inputStatus",name,
                [&] { session.selectProperty(QStringLiteral("event1"),QStringLiteral("pointerAcceleration"),0.25); },[&] { backend->fail(); }); QCOMPARE(backend->writes,1);
        } else if (page==QLatin1String("background")) {
            auto *backend=new BackgroundFixture; WallpaperSettings session(backend); QTRY_VERIFY(session.isReady()); LayoutShell shell(size);
            checkPage(shell,new DesktopBackgroundPage(LayoutShell::sidebar(),&session,nullptr),session,"applyWallpaper","revertWallpaper","wallpaperStatus",name,
                [&] { session.selectFillMode(1); },[&] { backend->fail(); }); QCOMPARE(backend->writes,1);
        } else if (page==QLatin1String("night-light")) {
            auto *backend=new LightFixture; NightLightSettings session(backend); QTRY_VERIFY(session.isReady()); LayoutShell shell(size);
            checkPage(shell,new NightLightPage(LayoutShell::sidebar(),&session,nullptr),session,"nightLightApply","nightLightRevert","nightLightMessage",name,
                [&] { session.selectValue(NightLightKey::NightTemperature,3500); },[&] { backend->fail(); }); QCOMPARE(backend->writes,1);
        } else if (page==QLatin1String("power")) {
            auto *backend=new PolicyFixture; PowerPolicySettings session(backend); QTRY_VERIFY(session.isReady());
            PowerProfileService profiles(std::make_unique<ProfilesFixture>()); LayoutShell shell(size);
            checkPage(shell,new PowerOptionsPage(LayoutShell::sidebar(),&session,&profiles,nullptr),session,"powerPolicyApply","powerPolicyRevert","powerPolicyStatus",name,
                [&] { session.selectDisplayTimeout(QStringLiteral("AC"),1200); },[&] { backend->fail(); }); QCOMPARE(backend->writes,1);
        } else {
            auto *backend=new ThemeFixture; AeroThemeSettings session(backend); QTRY_VERIFY(!session.isBusy()); LayoutShell shell(size);
            checkPage(shell,new PersonalizationPage(LayoutShell::sidebar(),&session,nullptr),session,"applyAeroTheme","revertAeroTheme","aeroThemeMessage",name,
                [&] { session.selectValue(QStringLiteral("aeroIntensity"),60); },[&] { backend->fail(); }); QCOMPARE(backend->writes,1);
        }
    }
    void soundImmediateActionsAndErrorsAtOriginalDialogSize() {
        auto *backend=new IntegrationAudioBackend; AudioSettings settings(backend); QTRY_VERIFY(settings.isReady());
        SoundDialog dialog(&settings); dialog.show(); dialog.activateWindow(); QCoreApplication::processEvents();
        QVERIFY(dialog.width()<=1000); QVERIFY(dialog.height()<=1080); qInfo()<<"Sound fixture actual size"<<dialog.size();
        capture(dialog,QStringLiteral("rc-primary-sound-overview"));
        auto *setVolume=dialog.findChild<QPushButton *>(QStringLiteral("playbackSetVolume")); QVERIFY(setVolume); QVERIFY(setVolume->isEnabled());
        tabMoves(*setVolume,dialog); settings.requestChange(audioVolumeRequest(settings.snapshot(),40)); QVERIFY(settings.isBusy()); QVERIFY(!setVolume->isEnabled());
        backend->complete(AudioOutcome::Failed,false); QVERIFY(!settings.isBusy());
        auto *status=dialog.findChild<QLabel *>(QStringLiteral("audioStatus")); QVERIFY(status); QVERIFY(status->wordWrap()); QVERIFY(status->text().contains(QStringLiteral("failed")));
        auto *buttons=dialog.findChild<QDialogButtonBox *>(QStringLiteral("audioDialogButtons")); QVERIFY(buttons); QVERIFY(!buttons->button(QDialogButtonBox::Apply));
        const QRect visible=dialog.rect(); QVERIFY(visible.contains(QRect(status->mapTo(&dialog,QPoint(0,0)),status->size())));
        auto *close=buttons->button(QDialogButtonBox::Close); QVERIFY(close); QVERIFY(visible.contains(QRect(close->mapTo(&dialog,QPoint(0,0)),close->size())));
        capture(dialog,QStringLiteral("rc-primary-sound-error")); QCOMPARE(backend->writes,1);
    }
};

int main(int argc,char **argv) {
    if (!qEnvironmentVariableIsSet("CONTROL_RC_PRIMARY_UI_CHILD")) {
        QCoreApplication app(argc,argv); QTemporaryDir sandbox; if (!sandbox.isValid()) return 2;
        const auto config=sandbox.filePath(QStringLiteral("config")),defaults=sandbox.filePath(QStringLiteral("defaults")),cache=sandbox.filePath(QStringLiteral("cache")),data=sandbox.filePath(QStringLiteral("data"));
        for (const auto &path:{config,defaults,cache,data}) if (!QDir().mkpath(path)) return 3;
        auto environment=QProcessEnvironment::systemEnvironment(); environment.insert("XDG_CONFIG_HOME",config); environment.insert("XDG_CONFIG_DIRS",defaults);
        environment.insert("XDG_CACHE_HOME",cache); environment.insert("XDG_DATA_HOME",data); environment.insert("KDE_SKIP_KDERC","1");
        environment.insert("CONTROL_RC_PRIMARY_UI_CHILD","1"); environment.insert("QT_SCALE_FACTOR","1");
        environment.remove("QT_SCREEN_SCALE_FACTORS"); environment.remove("QT_AUTO_SCREEN_SCALE_FACTOR");
        QProcess child; child.setProcessEnvironment(environment); child.setProcessChannelMode(QProcess::ForwardedChannels);
        QStringList args{QStringLiteral("--"),app.applicationFilePath()}; args<<app.arguments().mid(1); child.start(QStringLiteral("/usr/bin/dbus-run-session"),args);
        if (!child.waitForStarted(3000) || !child.waitForFinished(120000)) { child.kill(); child.waitForFinished(1000); return 4; }
        return child.exitStatus()==QProcess::NormalExit?child.exitCode():5;
    }
    // Every provider is injected. Even accidental bus discovery is confined to
    // this child's private bus; no real session/system service is reachable.
    qputenv("DBUS_SYSTEM_BUS_ADDRESS",qgetenv("DBUS_SESSION_BUS_ADDRESS"));
    QApplication app(argc,argv); app.setOrganizationName(QStringLiteral("linux-control-tests")); app.setApplicationName(QStringLiteral("rc-primary-ui"));
    Aero::registerStylesheet(&app); RcPrimaryUiTest test; return QTest::qExec(&test,argc,argv);
}
#include "rc_primary_ui_test.moc"
