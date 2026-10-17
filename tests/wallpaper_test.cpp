#include "settings/WallpaperSettings.h"
#include "platform/kde/WallpaperBackend.h"
#include "ui/pages/DesktopBackgroundPage.h"
#include <QtTest>
#include <QComboBox>
#include <QFile>
#include <QJSEngine>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QDBusConnection>

static WallpaperDesktop testDesktop(quint32 id = 1)
{
    WallpaperDesktop desktop; desktop.id = id; desktop.screen = int(id - 1);
    desktop.plugin = QStringLiteral("org.kde.image"); desktop.image = QStringLiteral("file:///old.png");
    return desktop;
}
class FakeWallpaperBackend final : public WallpaperBackend {
public:
    WallpaperState current{{testDesktop(), testDesktop(2)}};
    WallpaperDesktop requested;
    int writes = 0; bool hold = false, readFails = false, ignoreWrite = false, writeSuccess = true;
    QString image; int mode = 2;
    void readCurrent() override { emit readFinished(!readFails, current, readFails ? "read failed" : ""); }
    void writePicture(const WallpaperDesktop &expected, const QString &value, int fillMode) override {
        ++writes; requested = expected; image = value; mode = fillMode; if (!hold) finish();
    }
    void finish() {
        if (!ignoreWrite) for (auto &desktop : current.desktops) if (desktop.id == requested.id) {
            desktop.image = image; desktop.fillMode = mode; desktop.plugin = QStringLiteral("org.kde.image");
        }
        emit writeFinished(writeSuccess, writeSuccess ? "" : "write failed");
    }
    void changed() { emit externalChange(); }
};
class FakeWallpaperTransport final : public WallpaperScriptTransport {
public:
    QString script, output = QStringLiteral("[]"); bool success = true;
    void evaluate(const QString &value) override { script = value; emit finished(success, output, success ? "" : "service failed"); }
};

class WallpaperTest final : public QObject {
    Q_OBJECT
private slots:
    void sessionStagesAndVerifiesOnlySelectedDesktop() {
        auto *backend = new FakeWallpaperBackend; WallpaperSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); QVERIFY(session.canApply());
        session.selectDesktop(2); session.selectPicture(QStringLiteral("/new picture.png")); session.selectFillMode(1);
        QCOMPARE(backend->writes, 0); QVERIFY(session.isDirty());
        session.selectDesktop(1); QCOMPARE(session.selectedDesktop(), quint32(2));
        QSignalSpy finished(&session, &SettingsSession::applyFinished); session.apply();
        QCOMPARE(finished.count(), 1); QVERIFY(finished.first().first().toBool());
        QVERIFY(!session.isDirty()); QCOMPARE(backend->writes, 1);
        QCOMPARE(backend->current.desktops.first().image, QStringLiteral("file:///old.png"));
        QCOMPARE(backend->current.desktops.last().image, QStringLiteral("file:///new%20picture.png"));
        session.selectFillMode(2); session.revert();
        QVERIFY(!session.isDirty()); QCOMPARE(session.stagedFillMode(), 1); QCOMPARE(backend->writes, 1);
    }
    void preflightProtectsOutsideEditsAndDisappearance() {
        auto *backend = new FakeWallpaperBackend; WallpaperSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); session.selectPicture(QStringLiteral("/new.png"));
        backend->current.desktops.first().image = QStringLiteral("file:///outside.png");
        session.apply(); QCOMPARE(backend->writes, 0); QVERIFY(session.hasConflict()); QVERIFY(session.isDirty());
        session.revert(); QVERIFY(!session.hasConflict()); QCOMPARE(session.stagedImage(), QStringLiteral("file:///outside.png"));
        session.selectFillMode(1); backend->current.desktops.removeFirst(); backend->changed();
        QVERIFY(session.hasConflict()); QVERIFY(!session.canApply()); QVERIFY(session.isDirty());
        session.revert(); QCOMPARE(session.selectedDesktop(), quint32(2));
    }
    void failedReadbackNeverClaimsSuccess() {
        auto *backend = new FakeWallpaperBackend; WallpaperSettings session(backend);
        QTRY_VERIFY(!session.isBusy()); backend->ignoreWrite = true;
        session.selectPicture(QStringLiteral("/new.png")); QSignalSpy finished(&session, &SettingsSession::applyFinished);
        session.apply(); QVERIFY(!finished.first().first().toBool()); QVERIFY(session.hasConflict());
        QVERIFY(!session.errorText().isEmpty()); backend->changed(); QVERIFY(!session.errorText().isEmpty());
        session.revert(); QVERIFY(session.errorText().isEmpty());
    }
    void pendingWorkOutlivesView() {
        auto *backend = new FakeWallpaperBackend; WallpaperSettings session(backend);
        QTRY_VERIFY(!session.isBusy());
        auto *page = new DesktopBackgroundPage(new QScrollArea, &session, nullptr);
        session.selectPicture(QStringLiteral("/new.png")); backend->hold = true; session.apply();
        QVERIFY(session.isBusy()); delete page; backend->finish();
        QVERIFY(!session.isBusy()); QVERIFY(!session.isDirty());
    }
    void nativeControlsGateUnknownAndImmutableDesktop() {
        auto *backend = new FakeWallpaperBackend; backend->current.desktops.first().writable = false;
        WallpaperSettings session(backend); QTRY_VERIFY(!session.isBusy());
        DesktopBackgroundPage page(new QScrollArea, &session, nullptr);
        QVERIFY(!page.findChild<QPushButton *>("browseWallpaper")->isEnabled());
        QVERIFY(!page.findChild<QPushButton *>("applyWallpaper")->isEnabled());
        session.selectDesktop(2); QVERIFY(page.findChild<QPushButton *>("browseWallpaper")->isEnabled());
        session.selectFillMode(0); QVERIFY(page.findChild<QPushButton *>("applyWallpaper")->isEnabled());
    }
    void protocolRejectsAmbiguousAndOversizedReplies() {
        WallpaperState state; QString error;
        const QString json = QStringLiteral("[{\"id\":1,\"screen\":0,\"plugin\":\"org.kde.image\",\"image\":\"file:///a.png\",\"fillMode\":2,\"immutability\":1}]");
        QVERIFY(parseWallpaperState(json, state, error)); QCOMPARE(state.desktops.size(), 1);
        QVERIFY(!parseWallpaperState(json.left(json.size() - 1) + ',' + json.mid(1), state, error));
        QVERIFY(!parseWallpaperState(QString(65537, ' '), state, error));
        QString invalid = json; invalid.replace("\"id\":1", "\"id\":1.5"); QVERIFY(!parseWallpaperState(invalid, state, error));
        invalid = json; invalid.replace("\"immutability\":1", "\"immutability\":3"); QVERIFY(!parseWallpaperState(invalid, state, error));
        invalid = json; invalid.replace("\"immutability\":1", "\"immutability\":4"); QVERIFY(parseWallpaperState(invalid, state, error));
    }
    void scriptEscapesDataAndRefusesRacingEdits() {
        const auto baseline = testDesktop();
        const QString image = QStringLiteral("file:///evil'\"\\\n%20.png") + QChar(0x2028) + QStringLiteral("; hacked = true;");
        const QString fixture = QStringLiteral(
            "var writes = [], printed = '', hacked = false; function immutability(){return 'mutable';} "
            "function print(x){printed = x;} "
            "var first = {id:1, screen:0, wallpaperPlugin:'org.kde.image', currentConfigGroup:[], "
            "readConfig:function(k,v){ if(k==='Image') return 'file:///old.png'; if(k==='FillMode')return 2; return v; }, "
            "writeConfig:function(k,v){writes.push({key:k,value:v});}}; "
            "var other = {id:2, screen:1, writeConfig:function(){throw new Error('wrong desktop');}}; "
            "function desktops(){return [first,other];}");
        QJSEngine engine; QVERIFY(!engine.evaluate(fixture).isError());
        auto result = engine.evaluate(wallpaperWriteScript(baseline, image, 6)); QVERIFY2(!result.isError(), qPrintable(result.toString()));
        QCOMPARE(engine.globalObject().property("writes").property("length").toInt(), 2);
        QCOMPARE(engine.globalObject().property("writes").property(0).property("value").toString(), image);
        QVERIFY(!engine.globalObject().property("hacked").toBool());
        QJSEngine racing; racing.evaluate(fixture); racing.evaluate("first.screen = 3;");
        QVERIFY(racing.evaluate(wallpaperWriteScript(baseline, image, 2)).isError());
        QCOMPARE(racing.globalObject().property("writes").property("length").toInt(), 0);
        QJSEngine locked; locked.evaluate(fixture); locked.evaluate("immutability = function(){return 'systemImmutable';};");
        auto immutableBaseline = baseline; immutableBaseline.immutability = 4;
        QVERIFY(locked.evaluate(wallpaperWriteScript(immutableBaseline, image, 2)).isError());
        QCOMPARE(locked.globalObject().property("writes").property("length").toInt(), 0);
    }
    void backendValidatesImageBeforeSendingWrites() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        auto *transport = new FakeWallpaperTransport;
        KdeWallpaperBackend backend(transport, dir.filePath("desktoprc"));
        QSignalSpy writes(&backend, &WallpaperBackend::writeFinished);
        backend.writePicture(testDesktop(), QStringLiteral("https://example.com/picture.png"), 2);
        QCOMPARE(writes.count(), 1); QVERIFY(!writes.first().first().toBool()); QVERIFY(transport->script.isEmpty());
        QImage picture(20, 20, QImage::Format_RGB32); picture.fill(Qt::blue);
        const QString file = dir.filePath(QStringLiteral("picture'with quote.png")); QVERIFY(picture.save(file));
        const QString url = QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded);
        QString error; QVERIFY(validateWallpaperPicture(url, error));
        transport->output = QStringLiteral("background-requested"); backend.writePicture(testDesktop(), url, 1);
        QCOMPARE(writes.count(), 2); QVERIFY(writes.last().first().toBool()); QVERIFY(transport->script.contains("writeConfig"));
        QFile policy(dir.filePath("desktoprc")); QVERIFY(policy.open(QIODevice::WriteOnly));
        policy.write("[Containments][1][Wallpaper][org.kde.image][General]\nImage[$i]=locked.png\n"); policy.close();
        transport->script.clear(); backend.writePicture(testDesktop(), url, 1);
        QVERIFY(!writes.last().first().toBool()); QVERIFY(transport->script.isEmpty());
    }
    void disconnectedBusCompletesExactlyOnce() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        KdeWallpaperBackend backend(createWallpaperScriptTransport(QDBusConnection(QStringLiteral("disconnected-wallpaper-test"))), dir.filePath("desktoprc"));
        QSignalSpy replies(&backend, &WallpaperBackend::readFinished); backend.readCurrent();
        QTRY_COMPARE(replies.count(), 1); QVERIFY(!replies.first().first().toBool());
        QTest::qWait(30); QCOMPARE(replies.count(), 1);
    }
};
QTEST_MAIN(WallpaperTest)
#include "wallpaper_test.moc"
