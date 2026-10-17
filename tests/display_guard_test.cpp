#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTemporaryDir>
#include "display_fixture.h"

#ifndef CONTROL_DISPLAY_GUARD_TEST_PATH
#error Must use the test guard, never the production guard
#endif

class DisplayGuardTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QString statePath;
    QProcess guard;
    QByteArray pending;
    void save(QJsonObject state)
    {
        QSaveFile file(statePath); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(state).toJson()); QVERIFY(file.commit());
    }
    QJsonObject state()
    {
        QFile file(statePath); if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
    QString activeMode()
    {
        return state().value("config").toObject().value("outputs").toArray()[0].toObject()
            .value("currentModeId").toString();
    }
    void start(const DisplaySnapshot &baseline=displayFixture(), DisplaySnapshot requested={})
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("CONTROL_DISPLAY_TEST_STATE", statePath);
        guard.setProcessEnvironment(environment);
        guard.start(QString::fromUtf8(CONTROL_DISPLAY_GUARD_TEST_PATH), {});
        QVERIFY(guard.waitForStarted(2000));
        if(!requested.isValid()){requested=baseline;requested.output(1)->currentMode="2";requested=DisplayProtocol::normalized(requested);}
        guard.write(QJsonDocument(QJsonObject{{"baseline", baseline.json},
                    {"requested", requested.json}}).toJson(QJsonDocument::Compact) + '\n');
        QVERIFY(guard.waitForBytesWritten(2000));
    }
    QJsonObject message(int timeout=4000)
    {
        QElapsedTimer timer; timer.start();
        while (!pending.contains('\n') && timer.elapsed() < timeout) {
            guard.waitForReadyRead(50); pending += guard.readAllStandardOutput();
        }
        const int newline = pending.indexOf('\n');
        if (newline < 0) return {};
        auto result = QJsonDocument::fromJson(pending.left(newline)).object();
        pending.remove(0, newline + 1);
        return result;
    }
private slots:
    void init()
    {
        QVERIFY(directory.isValid()); statePath = directory.filePath("state.json");
        pending.clear(); save({{"config", displayFixture().json}, {"writes", 0}});
    }
    void cleanup()
    {
        guard.closeWriteChannel();
        if (guard.state() != QProcess::NotRunning) QVERIFY(guard.waitForFinished(4000));
    }
    void keepIsVerified()
    {
        start(); QCOMPARE(message().value("event").toString(), QStringLiteral("preview"));
        QCOMPARE(activeMode(), QStringLiteral("2"));
        guard.write("keep\n");
        const auto final = message(); QVERIFY(final.value("kept").toBool());
        QCOMPARE(activeMode(), QStringLiteral("2")); QCOMPARE(state().value("writes").toInt(), 1);
    }
    void timeoutRestoresWithoutUiHelp()
    {
        start(); QCOMPARE(message().value("event").toString(), QStringLiteral("preview"));
        const auto final = message(); QVERIFY(final.value("restored").toBool());
        QCOMPARE(activeMode(), QStringLiteral("1")); QCOMPARE(state().value("writes").toInt(), 2);
    }
    void parentPipeEofRestores()
    {
        start(); QCOMPARE(message().value("event").toString(), QStringLiteral("preview"));
        guard.closeWriteChannel();
        const auto final = message(); QVERIFY(final.value("restored").toBool());
        QCOMPARE(activeMode(), QStringLiteral("1"));
    }
    void explicitRevertRestores()
    {
        start(); QCOMPARE(message().value("event").toString(), QStringLiteral("preview"));
        guard.write("revert\n"); QVERIFY(message().value("restored").toBool());
        QCOMPARE(activeMode(), QStringLiteral("1"));
    }
    void conflictIsNotOverwritten()
    {
        start(); QCOMPARE(message().value("event").toString(), QStringLiteral("preview"));
        auto changed = state();
        const auto snapshot = displayModified(displayFixture("2"), "scale", 2);
        changed.insert("config", snapshot.json); save(changed);
        guard.write("keep\n");
        const auto final = message(); QVERIFY(!final.value("kept").toBool());
        QVERIFY(!final.value("restored").toBool()); QVERIFY(!final.value("error").toString().isEmpty());
        QCOMPARE(state().value("writes").toInt(), 1);
    }
    void brightnessChangeStillAllowsModeRollback()
    {
        start(); QCOMPARE(message().value("event").toString(), QStringLiteral("preview"));
        auto changed = state();
        changed.insert("config", displayModified(displayFixture("2"), "brightness", 0.5).json);
        save(changed); guard.write("revert\n");
        QVERIFY(message().value("restored").toBool());
        QCOMPARE(activeMode(), QStringLiteral("1"));
        const auto output = state().value("config").toObject().value("outputs").toArray()[0].toObject();
        QCOMPARE(output.value("brightness").toDouble(), 0.5);
    }
    void falseCommandSuccessFailsReadback()
    {
        auto fixture = state(); fixture.insert("mismatch", true); save(fixture);
        start(); const auto final = message();
        QCOMPARE(final.value("event").toString(), QStringLiteral("finished"));
        QVERIFY(!final.value("kept").toBool()); QVERIFY(final.value("restored").toBool());
        QCOMPARE(activeMode(), QStringLiteral("1"));
    }
    void multiOutputBatchAndRollbackPreserveFreshFields()
    {
        auto baseline=displayMultiFixture();auto target=baseline;
        target.output(1)->currentMode="2";target.output(2)->scale=1.25;
        target.output(2)->rotation=2;target.output(2)->position=QPoint(1280,0);
        target=DisplayProtocol::normalized(target);
        save({{"config",baseline.json},{"writes",0}});start(baseline,target);
        QCOMPARE(message().value("event").toString(),QStringLiteral("preview"));
        QCOMPARE(state().value("writes").toInt(),1);
        const auto batch=state().value("batches").toArray()[0].toArray();QVERIFY(batch.size()>=4);
        auto changed=state();auto config=changed.value("config").toObject();auto outputs=config.value("outputs").toArray();
        auto output=outputs[1].toObject();output.insert("brightness",0.2);output.insert("vrrPolicy",2);outputs[1]=output;config.insert("outputs",outputs);changed.insert("config",config);save(changed);
        guard.write("revert\n");QVERIFY(message().value("restored").toBool());
        DisplaySnapshot restored;QString error;QVERIFY(DisplayProtocol::parse(QJsonDocument(state().value("config").toObject()).toJson(),restored,error));
        QVERIFY(DisplayProtocol::same(restored,baseline));
        const auto native=restored.json.value("outputs").toArray()[1].toObject();QCOMPARE(native.value("brightness").toDouble(),0.2);QCOMPARE(native.value("vrrPolicy").toInt(),2);
    }
    void partialNativeFailureIsRolledBack()
    {
        const auto baseline=displayMultiFixture();auto target=baseline;
        target.output(1)->currentMode="2";target.output(2)->scale=1.5;target.output(2)->rotation=8;target=DisplayProtocol::normalized(target);
        save({{"config",baseline.json},{"writes",0},{"partialArguments",1},{"failFirstWrite",true}});
        start(baseline,target);const auto final=message();
        QCOMPARE(final.value("event").toString(),QStringLiteral("finished"));
        QVERIFY(final.value("restored").toBool());QVERIFY(!final.value("kept").toBool());
        QCOMPARE(state().value("writes").toInt(),2);
        DisplaySnapshot restored;QString error;QVERIFY(DisplayProtocol::parse(QJsonDocument(state().value("config").toObject()).toJson(),restored,error));QVERIFY(DisplayProtocol::same(restored,baseline));
    }
    void partialEnablementAndPriorityCompactionAreRolledBack()
    {
        const auto baseline=displayMultiFixture();auto target=baseline;
        target.output(1)->enabled=false;target.output(2)->scale=1.5;target=DisplayProtocol::normalized(target);
        save({{"config",baseline.json},{"writes",0},{"partialArguments",1},{"failFirstWrite",true}});
        start(baseline,target);const auto final=message();QVERIFY(final.value("restored").toBool());
        DisplaySnapshot restored;QString error;QVERIFY(DisplayProtocol::parse(QJsonDocument(state().value("config").toObject()).toJson(),restored,error));QVERIFY(DisplayProtocol::same(restored,baseline));
    }
    void threeOutputPartialOriginShiftIsRolledBack()
    {
        const auto baseline=displayTripleFixture();auto target=baseline;
        target.output(1)->enabled=false;target.output(2)->position=QPoint(0,0);
        target.output(3)->position=QPoint(3000,0);target=DisplayProtocol::normalized(target);
        // The first native argument disables A. Native origin normalization
        // leaves B0/C1920, although requested C is3000 and baseline C is3840.
        save({{"config",baseline.json},{"writes",0},{"partialArguments",1},{"failFirstWrite",true}});
        start(baseline,target);const auto final=message();
        QCOMPARE(final.value("event").toString(),QStringLiteral("finished"));
        QVERIFY(final.value("restored").toBool());QVERIFY(!final.value("kept").toBool());
        QCOMPARE(state().value("writes").toInt(),2);
        DisplaySnapshot restored;QString error;
        QVERIFY(DisplayProtocol::parse(QJsonDocument(state().value("config").toObject()).toJson(),restored,error));
        QVERIFY(DisplayProtocol::same(restored,baseline));
        const auto batches=state().value("batches").toArray();
        QVERIFY(batches[1].toArray().contains(QJsonValue("output.DP-2.position.3840,0")));
    }
    void hotplugBeforeCommitDoesNotWrite()
    {
        const auto baseline=displayMultiFixture();auto changed=baseline;
        changed=displayModified(changed,"name","DP-7");save({{"config",changed.json},{"writes",0}});
        start(baseline);const auto final=message();QVERIFY(!final.value("kept").toBool());QVERIFY(!final.value("restored").toBool());QCOMPARE(state().value("writes").toInt(),0);
    }
    void allOffRequestIsRejectedWithoutNativeWrite()
    {
        const auto baseline=displayMultiFixture();auto target=baseline;
        target.output(1)->enabled=false;target.output(2)->enabled=false;target=DisplayProtocol::normalized(target);
        save({{"config",baseline.json},{"writes",0}});start(baseline,target);
        const auto final=message();QVERIFY(!final.value("kept").toBool());QVERIFY(!final.value("restored").toBool());QCOMPARE(state().value("writes").toInt(),0);
    }
    void writerTimeoutDoesNotClaimCertainRestoration()
    {
        save({{"config",displayFixture().json},{"writes",0},{"hangFirstWrite",true}});
        start();const auto final=message(10000);
        QCOMPARE(final.value("event").toString(),QStringLiteral("finished"));
        QVERIFY(!final.value("kept").toBool());QVERIFY(!final.value("restored").toBool());
        QCOMPARE(activeMode(),QStringLiteral("1"));QCOMPARE(state().value("writes").toInt(),2);
    }
    void doctorEnvironmentDoesNotInheritOffscreenBackend()
    {
        const auto priorType=qgetenv("XDG_SESSION_TYPE"),priorDisplay=qgetenv("WAYLAND_DISPLAY"),priorPlatform=qgetenv("QT_QPA_PLATFORM"),priorBackend=qgetenv("KSCREEN_BACKEND");
        qputenv("XDG_SESSION_TYPE","wayland");qputenv("WAYLAND_DISPLAY","fixture-only");qputenv("QT_QPA_PLATFORM","offscreen");qputenv("KSCREEN_BACKEND","XRandR");
        start();const auto preview=message();guard.write("revert\n");const auto final=message();
        qputenv("XDG_SESSION_TYPE",priorType);qputenv("WAYLAND_DISPLAY",priorDisplay);qputenv("QT_QPA_PLATFORM",priorPlatform);qputenv("KSCREEN_BACKEND",priorBackend);
        QCOMPARE(preview.value("event").toString(),QStringLiteral("preview"));QVERIFY(final.value("restored").toBool());
        QCOMPARE(state().value("doctorPlatform").toString(),QStringLiteral("wayland"));QVERIFY(state().value("backendOverride").toString().isEmpty());
    }
    void unrelatedModeTransactionPreservesSavedRawScale()
    {
        const auto baseline=displayModified(displayMultiFixture(),"scale",4.01);
        auto target=baseline;target.output(2)->currentMode="2";target=DisplayProtocol::normalized(target);
        save({{"config",baseline.json},{"writes",0}});start(baseline,target);
        QCOMPARE(message().value("event").toString(),QStringLiteral("preview"));
        QCOMPARE(state().value("config").toObject().value("outputs").toArray()[0].toObject().value("scale").toDouble(),4.01);
        guard.write("revert\n");QVERIFY(message().value("restored").toBool());
        DisplaySnapshot restored;QString error;
        QVERIFY(DisplayProtocol::parse(QJsonDocument(state().value("config").toObject()).toJson(),restored,error));
        QVERIFY(DisplayProtocol::same(restored,baseline));QCOMPARE(restored.output(1)->scale,4.01);
        for(const auto &batch:state().value("batches").toArray())for(const auto &argument:batch.toArray())
            QVERIFY(!argument.toString().startsWith("output.eDP-1.scale."));
    }
    void smallOutsideScaleChangeStopsRestoration()
    {
        const auto baseline=displayModified(displayMultiFixture(),"scale",4.01);
        auto target=baseline;target.output(2)->currentMode="2";target=DisplayProtocol::normalized(target);
        save({{"config",baseline.json},{"writes",0}});start(baseline,target);
        QCOMPARE(message().value("event").toString(),QStringLiteral("preview"));
        auto changed=state();changed.insert("config",displayModified(target,"scale",4.0100000001).json);save(changed);
        guard.write("revert\n");const auto final=message();
        QVERIFY(!final.value("restored").toBool());QVERIFY(!final.value("kept").toBool());
        QCOMPARE(state().value("writes").toInt(),1);
        QCOMPARE(state().value("config").toObject().value("outputs").toArray()[0].toObject().value("scale").toDouble(),4.0100000001);
    }
    void nonRestorableSavedScaleReplacementDoesNotWrite()
    {
        const auto baseline=displayModified(displayFixture(),"scale",DisplayProtocol::MaximumReadScale);
        auto target=baseline;target.output(1)->scale=1.5;target=DisplayProtocol::normalized(target);
        save({{"config",baseline.json},{"writes",0}});start(baseline,target);
        const auto final=message();QVERIFY(!final.value("restored").toBool());QVERIFY(!final.value("kept").toBool());
        QVERIFY(final.value("error").toString().contains("restored exactly"));QCOMPARE(state().value("writes").toInt(),0);
    }
    void readOnlyHelperDoesNotWrite()
    {
        auto environment=QProcessEnvironment::systemEnvironment();environment.insert("CONTROL_DISPLAY_TEST_STATE",statePath);guard.setProcessEnvironment(environment);
        guard.start(QString::fromUtf8(CONTROL_DISPLAY_GUARD_TEST_PATH),{QStringLiteral("--read")});QVERIFY(guard.waitForFinished(3000));
        DisplaySnapshot read;QString error;QVERIFY(DisplayProtocol::parse(guard.readAllStandardOutput(),read,error));QVERIFY(DisplayProtocol::same(read,displayFixture()));QCOMPARE(state().value("writes").toInt(),0);
    }
    void unreadLargeNotificationCannotBlockRollback()
    {
        auto fixture = state();
        auto config = fixture.value("config").toObject();
        config.insert("padding", QString(512 * 1024, QLatin1Char('x')));
        fixture.insert("config", config); save(fixture);
        DisplaySnapshot baseline;QString error;
        QVERIFY(DisplayProtocol::parse(QJsonDocument(config).toJson(),baseline,error));
        start(baseline);
        // Both full request snapshots carry 512 KiB of unrelated native data:
        // this request is larger than the earlier one-snapshot 1 MiB bound.
        while(guard.bytesToWrite()>0) QVERIFY(guard.waitForBytesWritten(2000));
        // Intentionally do not run a Qt event loop or read the child's stdout.
        // Its preview frame exceeds pipe capacity, simulating a frozen UI.
        QTest::qSleep(2200);
        QCOMPARE(activeMode(), QStringLiteral("1"));
        QCOMPARE(state().value("writes").toInt(), 2);
        QVERIFY(guard.waitForFinished(2000));
    }
};
QTEST_GUILESS_MAIN(DisplayGuardTest)
#include "display_guard_test.moc"
