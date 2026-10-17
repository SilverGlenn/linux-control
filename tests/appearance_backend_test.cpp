#include <QtTest>
#include <QFile>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDBusConnection>
#include <KConfig>
#include <KConfigGroup>
#include "platform/kde/AppearanceBackend.h"

class FakeAppearanceReload : public AppearanceReloadService {
public:
    bool available = true;
    bool success = true;
    QStringList requests;
    bool isAvailable() const override { return available; }
    QString unavailableReason() const override { return QStringLiteral("No reload service"); }
    void requestReload(const QString &action) override
    {
        requests << action;
        emit finished(success, success ? QString() : QStringLiteral("Saved but live value unverified"));
    }
};

class AppearanceBackendTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<AppearanceState>(); }
    void init()
    {
        QVERIFY(temp.isValid());
        path = temp.filePath("kwinrc");
        QFile::remove(path);
        reload = new FakeAppearanceReload;
        backend = new KdeAppearanceBackend(path, reload);
    }
    void cleanup() { delete backend; }
    void defaultReadCreatesNoOverride()
    {
        QSignalSpy read(backend, &AppearanceBackend::readFinished);
        backend->readCurrent();
        QCOMPARE(read.size(), 1);
        QVERIFY(read[0][0].toBool());
        QCOMPARE(qvariant_cast<AppearanceState>(read[0][1]).doubleClickAction, "Maximize");
        QVERIFY(!QFile::exists(path));
        QVERIFY(reload->requests.isEmpty());
    }
    void disconnectedCapabilityProbeCompletesExactlyOnce()
    {
        // A named, never-connected handle cannot access the user's session bus.
        const QDBusConnection disconnected(QStringLiteral("appearance-disconnected-test"));
        QVERIFY(!disconnected.isConnected());
        auto *service = createKWinAppearanceReloadService(disconnected);
        KdeAppearanceBackend isolated(path, service);
        QSignalSpy changes(service, &AppearanceReloadService::availabilityChanged);
        QTRY_VERIFY(service->unavailableReason().contains("unavailable"));
        QVERIFY(!service->isAvailable());
        QCOMPARE(changes.size(), 2); // Checking, then one terminal unavailable state.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QCOMPARE(changes.size(), 2); // Finished signal and fallback never double-complete.
        QSignalSpy read(&isolated, &AppearanceBackend::readFinished);
        isolated.readCurrent();
        const auto state = qvariant_cast<AppearanceState>(read[0][1]);
        QVERIFY(!state.reloadAvailable);
        QVERIFY(!state.canApply());
        QVERIFY(!QFile::exists(path));
    }
    void singleKeyWritePreservesOtherPreferences()
    {
        put("[Windows]\nTitlebarDoubleClickCommand=Maximize\nFocusPolicy=FocusFollowsMouse\n[Unrelated]\nValue=keep\n");
        QSignalSpy finished(backend, &AppearanceBackend::writeFinished);
        backend->writeAction("Maximize", "Minimize");
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished[0][0].toBool());
        QCOMPARE(reload->requests, QStringList{"Minimize"});
        KConfig config(path, KConfig::SimpleConfig);
        QCOMPARE(KConfigGroup(&config, "Windows").readEntry("TitlebarDoubleClickCommand"), "Minimize");
        QCOMPARE(KConfigGroup(&config, "Windows").readEntry("FocusPolicy"), "FocusFollowsMouse");
        QCOMPARE(KConfigGroup(&config, "Unrelated").readEntry("Value"), "keep");
    }
    void immutableKeyDisablesAndRejectsWrite()
    {
        put("[Windows]\nTitlebarDoubleClickCommand[$i]=Maximize\n");
        QSignalSpy read(backend, &AppearanceBackend::readFinished);
        backend->readCurrent();
        QVERIFY(!qvariant_cast<AppearanceState>(read[0][1]).writable);
        QSignalSpy finished(backend, &AppearanceBackend::writeFinished);
        backend->writeAction("Maximize", "Nothing");
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(reload->requests.isEmpty());
        QVERIFY(contents().contains("TitlebarDoubleClickCommand[$i]=Maximize"));
    }
    void missingDaemonAndUnsupportedActionNeverWrite()
    {
        put("[Windows]\nTitlebarDoubleClickCommand=Maximize\n");
        const QByteArray before = contents();
        reload->available = false;
        backend->writeAction("Maximize", "Minimize");
        QCOMPARE(contents(), before);
        reload->available = true;
        backend->writeAction("Maximize", "Close");
        QCOMPARE(contents(), before);
        QVERIFY(reload->requests.isEmpty());
    }
    void immediatePreWriteCheckProtectsExternalEdit()
    {
        put("[Windows]\nTitlebarDoubleClickCommand=Nothing\n");
        backend->writeAction("Maximize", "Minimize");
        QVERIFY(reload->requests.isEmpty());
        QVERIFY(contents().contains("TitlebarDoubleClickCommand=Nothing"));
    }
    void failedRuntimeVerificationRetainsExplicitSavedState()
    {
        reload->success = false;
        QSignalSpy finished(backend, &AppearanceBackend::writeFinished);
        backend->writeAction("Maximize", "Minimize");
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(finished[0][1].toString().contains("Saved"));
        QSignalSpy read(backend, &AppearanceBackend::readFinished);
        backend->readCurrent();
        QCOMPARE(qvariant_cast<AppearanceState>(read[0][1]).doubleClickAction, "Minimize");
    }
    void atomicReplacementRemainsWatched()
    {
        put("[Windows]\nTitlebarDoubleClickCommand=Maximize\n");
        QSignalSpy changed(backend, &AppearanceBackend::externalChange);
        atomicPut("[Windows]\nTitlebarDoubleClickCommand=Nothing\n");
        QTRY_VERIFY(!changed.isEmpty());
        changed.clear();
        atomicPut("[Windows]\nTitlebarDoubleClickCommand=Minimize\n");
        QTRY_VERIFY(!changed.isEmpty());
        QSignalSpy read(backend, &AppearanceBackend::readFinished);
        backend->readCurrent();
        QCOMPARE(qvariant_cast<AppearanceState>(read[0][1]).doubleClickAction, "Minimize");
    }
    void runtimeDiagnosticParserIsBoundedAndVersionPinned()
    {
        int operation = 0;
        const QString report = QStringLiteral("Version\n=======\nKWin version: 6.6.6\n\nOptions\n=======\noperationTitlebarDblClick: 5002\n\nScreen Edges\n");
        QVERIFY(parseKWinAppearanceReport(report, operation));
        QCOMPARE(operation, 5002);
        QVERIFY(!parseKWinAppearanceReport(QString(report).replace("6.6.6", "6.7.0"), operation));
        QVERIFY(!parseKWinAppearanceReport(QString(report).replace("Options", "Other"), operation));
        QVERIFY(!parseKWinAppearanceReport(QString(report).replace("5002", "bad"), operation));
        QVERIFY(!parseKWinAppearanceReport(QString(65537, QLatin1Char('x')), operation));
        QVERIFY(!parseKWinAppearanceReport(QString(report).replace("\n\nScreen Edges", "\noperationTitlebarDblClick: 5000\n\nScreen Edges"), operation));
    }
private:
    void put(const QByteArray &text)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(text), qint64(text.size()));
        file.close();
    }
    void atomicPut(const QByteArray &text)
    {
        QSaveFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(text), qint64(text.size()));
        QVERIFY(file.commit());
    }
    QByteArray contents()
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }
    QTemporaryDir temp;
    QString path;
    FakeAppearanceReload *reload = nullptr;
    KdeAppearanceBackend *backend = nullptr;
};
QTEST_GUILESS_MAIN(AppearanceBackendTest)
#include "appearance_backend_test.moc"
