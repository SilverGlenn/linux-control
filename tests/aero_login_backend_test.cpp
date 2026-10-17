#include <QtTest>
#include "aero_login_fixture.h"
#include "platform/kde/AeroLoginBackend.h"
#include "login/AeroLoginProtocol.h"
#include <QCryptographicHash>
#include <QMutex>
#include <QSemaphore>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <memory>

class FakeAeroLoginAuth final:public AeroLoginAuthRunner {
public:
    int starts=0;quint64 token=0;QByteArray request,overrideReceipt;bool recovery=false;std::function<QByteArray()> receipt;
    void start(quint64 operation,const QByteArray &bytes,bool recover) override{++starts;token=operation;request=bytes;recovery=recover;}
    void complete(bool process,bool saved,const QString &helperError={},bool pending=false,const QString &transportError={})
    {emit finished(token,process,AeroLoginProtocol::encodeReply(saved,helperError,pending,!overrideReceipt.isEmpty()?overrideReceipt:saved&&receipt?receipt():QByteArray{}),transportError);}
};
class AeroLoginBackendTest final:public QObject {
    Q_OBJECT
    struct State {
        QMutex mutex;AeroLoginSnapshot snapshot;AeroLoginHelperCapability capability{true,{},"approved-helper-v1"};QString readError;
        std::atomic_int reads{0},finishedReads{0};std::atomic_bool hold{false};std::atomic<QThread *> readerThread{nullptr};QSemaphore gate;
    };
    std::shared_ptr<State> data;FakeAeroLoginAuth *runner=nullptr;KdeAeroLoginBackend *backend=nullptr;quint64 token=0;QTemporaryDir directory;
    void makeBackend()
    {
        runner=new FakeAeroLoginAuth;const auto shared=data;
        runner->receipt=[shared]{QMutexLocker lock(&shared->mutex);return shared->snapshot.revision;};
        backend=new KdeAeroLoginBackend(runner,[shared](QString &error){
            ++shared->reads;shared->readerThread=QThread::currentThread();if(shared->hold)shared->gate.acquire();QMutexLocker lock(&shared->mutex);error=shared->readError;auto snapshot=shared->snapshot;++shared->finishedReads;return snapshot;
        },[shared]{QMutexLocker lock(&shared->mutex);return shared->capability;},{directory.filePath("fixed-watched-file")});
    }
    AeroLoginSnapshot read()
    {
        QSignalSpy done(backend,&AeroLoginBackend::readFinished);backend->readCurrent(++token);if(done.isEmpty())done.wait(5000);
        if(done.size()!=1||!done[0][1].toBool())return {};return qvariant_cast<AeroLoginSnapshot>(done[0][2]);
    }
    AeroLoginSnapshot request(const AeroLoginSnapshot &before)
    {auto requested=before;QString error;const bool staged=AeroLoginTypes::stage(requested,before,AeroLoginKey::StartupAnimation,true,error);if(!staged)return {};return requested;}
    void saveFake(const AeroLoginSnapshot &requested)
    {QMutexLocker lock(&data->mutex);data->snapshot=savedAeroLoginFixture(requested);}
private slots:
    void initTestCase(){qRegisterMetaType<AeroLoginSnapshot>();}
    void init(){QVERIFY(directory.isValid());data=std::make_shared<State>();data->snapshot=aeroLoginFixture();data->snapshot.revision=QCryptographicHash::hash("native-source-fixture",QCryptographicHash::Sha256);token=0;makeBackend();}
    void cleanup(){data->hold=false;data->gate.release(4);delete backend;backend=nullptr;runner=nullptr;}
    void readsArePassiveOffTheGuiThreadAndEnrichHelperRevision()
    {
        const auto snapshot=read();QCOMPARE(snapshot.fields.size(),4);QVERIFY(data->readerThread.load()!=QCoreApplication::instance()->thread());QCOMPARE(snapshot.helperRevision,QByteArray("approved-helper-v1"));QVERIFY(snapshot.helperAvailable);QCOMPARE(runner->starts,0);
    }
    void heldFileReadLeavesEventLoopResponsive()
    {
        data->hold=true;QSignalSpy done(backend,&AeroLoginBackend::readFinished);backend->readCurrent(++token);QTRY_COMPARE(data->reads.load(),1);QCOMPARE(done.size(),0);
        bool tick=false;QTimer::singleShot(0,[&]{tick=true;});QTRY_VERIFY(tick);data->hold=false;data->gate.release();QTRY_COMPARE(done.size(),1);QVERIFY(done[0][1].toBool());QCOMPARE(runner->starts,0);
    }
    void readableUntrustedSourceAndMissingHelperRemainInspectable()
    {
        {QMutexLocker lock(&data->mutex);data->snapshot.trustedInstallation=data->snapshot.writable=false;data->snapshot.reason="Untrusted installation";data->capability={false,"Helper not installed",{}};}
        const auto snapshot=read();QCOMPARE(snapshot.fields.size(),4);QVERIFY(snapshot.sourceValid&&!snapshot.trustedInstallation&&!snapshot.helperAvailable);QCOMPARE(snapshot.helperReason,QStringLiteral("Helper not installed"));QCOMPARE(runner->starts,0);
    }
    void sourceOrHelperApprovalRaceStopsBeforeAuthorization()
    {
        const auto before=read(),requested=request(before);{QMutexLocker lock(&data->mutex);data->snapshot.revision+="external";}
        QSignalSpy done(backend,&AeroLoginBackend::writeFinished);backend->writeChanges(++token,before,requested);QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][1].toBool());QCOMPARE(runner->starts,0);
        {QMutexLocker lock(&data->mutex);data->snapshot.revision=before.revision;data->capability.revision="approved-helper-v2";}
        backend->writeChanges(++token,before,requested);QTRY_COMPARE(done.size(),2);QVERIFY(!done[1][1].toBool());QCOMPARE(runner->starts,0);
    }
    void onlyExplicitIntentReachesFakeAuthorizationAndFreshReadback()
    {
        const auto before=read(),requested=request(before);QSignalSpy done(backend,&AeroLoginBackend::writeFinished);backend->writeChanges(++token,before,requested);QTRY_COMPARE(runner->starts,1);QVERIFY(!runner->recovery);
        AeroLoginSnapshot decoded;bool recovery=false;QString error;QVERIFY2(AeroLoginProtocol::decodeRequest(runner->request,before,decoded,recovery,error),qPrintable(error));QVERIFY(!recovery);QCOMPARE(decoded.intents.size(),1);QVERIFY(decoded.intents.contains(AeroLoginKey::StartupAnimation));QCOMPARE(decoded.backgroundIntent,AeroLoginBackgroundIntent::Keep);
        saveFake(requested);runner->complete(true,true);QTRY_COMPARE(done.size(),1);QVERIFY(done[0][1].toBool());QVERIFY2(done[0][2].toString().isEmpty(),qPrintable(done[0][2].toString()));QVERIFY(data->reads>=3);
    }
    void selectedImageBytesSurviveAsyncPreflightAndAuthorization()
    {
        const auto before=read();auto requested=before;const auto selected=aeroLoginImageFixture(QColor(20,190,30));QString error;QVERIFY(AeroLoginTypes::selectImage(requested,before,selected,error));
        QSignalSpy done(backend,&AeroLoginBackend::writeFinished);backend->writeChanges(++token,before,requested);QTRY_COMPARE(runner->starts,1);
        AeroLoginSnapshot decoded;bool recovery=false;QVERIFY2(AeroLoginProtocol::decodeRequest(runner->request,before,decoded,recovery,error),qPrintable(error));QCOMPARE(decoded.replacement.bytes,selected.bytes);QCOMPARE(decoded.replacement.digest,selected.digest);
        saveFake(requested);runner->complete(true,true);QTRY_COMPARE(done.size(),1);QVERIFY(done[0][1].toBool());QVERIFY2(done[0][2].toString().isEmpty(),qPrintable(done[0][2].toString()));
    }
    void cancellationAndLostAcknowledgementKeepErrorsEvenWhenFilesMatch()
    {
        const auto before=read(),requested=request(before);QSignalSpy done(backend,&AeroLoginBackend::writeFinished);backend->writeChanges(++token,before,requested);QTRY_COMPARE(runner->starts,1);runner->complete(false,false,{},false,"Authorization cancelled");QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][1].toBool());QVERIFY(done[0][2].toString().contains("cancelled"));
        backend->writeChanges(++token,before,requested);QTRY_COMPARE(runner->starts,2);saveFake(requested);emit runner->finished(runner->token,false,{},"Helper acknowledgement lost");QTRY_COMPARE(done.size(),2);QVERIFY(!done[1][1].toBool());QVERIFY(done[1][2].toString().contains("lost"));
    }
    void delayedJobRetainsSingleOperationUntilKnownTerminal()
    {
        const auto before=read(),requested=request(before);QSignalSpy done(backend,&AeroLoginBackend::writeFinished),pending(backend,&AeroLoginBackend::operationPending);backend->writeChanges(++token,before,requested);QTRY_COMPARE(runner->starts,1);
        emit runner->delayed(runner->token-1,"stale");QCOMPARE(pending.size(),0);emit runner->delayed(runner->token,"Transaction still running; not cancelled");QCOMPARE(pending.size(),1);QCOMPARE(done.size(),0);
        emit runner->finished(runner->token-1,true,AeroLoginProtocol::encodeReply(true,{},false),{});QCOMPARE(done.size(),0);
        backend->writeChanges(999,before,requested);QCOMPARE(done.size(),1);QVERIFY(!done[0][1].toBool());QCOMPARE(runner->starts,1);
        saveFake(requested);runner->complete(true,true);QTRY_COMPARE(done.size(),2);QVERIFY(done[1][1].toBool());QVERIFY(done[1][2].toString().isEmpty());
    }
    void falseSuccessAndReportedRecoveryPendingNeverClearVerification()
    {
        const auto before=read(),requested=request(before);QSignalSpy done(backend,&AeroLoginBackend::writeFinished);backend->writeChanges(++token,before,requested);QTRY_COMPARE(runner->starts,1);runner->complete(true,true);QTRY_COMPARE(done.size(),1);QVERIFY(done[0][1].toBool());QVERIFY(!done[0][2].toString().isEmpty());
        backend->writeChanges(++token,before,requested);QTRY_COMPARE(runner->starts,2);saveFake(requested);runner->complete(true,true,{},true);QTRY_COMPARE(done.size(),2);QVERIFY(!done[1][2].toString().isEmpty());
    }
    void foreignUntouchedEditIsPreservedAndReportedAfterAuthorization()
    {
        const auto before=read(),requested=request(before);QSignalSpy done(backend,&AeroLoginBackend::writeFinished);backend->writeChanges(++token,before,requested);QTRY_COMPARE(runner->starts,1);saveFake(requested);
        {QMutexLocker lock(&data->mutex);data->snapshot.fields[AeroLoginKey::StartupSound].value=false;data->snapshot.fields[AeroLoginKey::StartupSound].raw="false";}
        runner->complete(true,true);QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][2].toString().isEmpty());QMutexLocker lock(&data->mutex);QVERIFY(!data->snapshot.fields[AeroLoginKey::StartupSound].value);
    }
    void commitReceiptDetectsForeignUnknownKeyRevisionChange()
    {
        const auto before=read(),requested=request(before);QSignalSpy done(backend,&AeroLoginBackend::writeFinished);backend->writeChanges(++token,before,requested);QTRY_COMPARE(runner->starts,1);saveFake(requested);
        {QMutexLocker lock(&data->mutex);runner->overrideReceipt=data->snapshot.revision;data->snapshot.revision=QCryptographicHash::hash(data->snapshot.revision+"unowned-key-edit",QCryptographicHash::Sha256);}
        runner->complete(true,true);QTRY_COMPARE(done.size(),1);QVERIFY(done[0][1].toBool());QVERIFY(done[0][2].toString().contains("revision"));QCOMPARE(done[0][3].toByteArray(),runner->overrideReceipt);
    }
    void explicitRecoveryUsesFreshRevisionAndNoAutomaticRetry()
    {
        {QMutexLocker lock(&data->mutex);data->snapshot.recoveryPending=data->snapshot.canRecover=true;data->snapshot.writable=false;}
        const auto before=read();QSignalSpy done(backend,&AeroLoginBackend::writeFinished);backend->recover(++token,before);QTRY_COMPARE(runner->starts,1);QVERIFY(runner->recovery);
        AeroLoginSnapshot decoded;bool recovery=false;QString error;QVERIFY2(AeroLoginProtocol::decodeRequest(runner->request,before,decoded,recovery,error),qPrintable(error));QVERIFY(recovery);
        {QMutexLocker lock(&data->mutex);data->snapshot.recoveryPending=data->snapshot.canRecover=false;data->snapshot.writable=true;data->snapshot.revision=QCryptographicHash::hash(data->snapshot.revision+"recovered",QCryptographicHash::Sha256);}
        runner->complete(true,true);QTRY_COMPARE(done.size(),1);QVERIFY(done[0][1].toBool());QVERIFY2(done[0][2].toString().isEmpty(),qPrintable(done[0][2].toString()));QCOMPARE(runner->starts,1);
    }
    void deletingBackendDoesNotInvalidateOffThreadReadState()
    {
        data->hold=true;backend->readCurrent(++token);QTRY_COMPARE(data->reads.load(),1);delete backend;backend=nullptr;runner=nullptr;data->hold=false;data->gate.release();QTRY_COMPARE(data->finishedReads.load(),1);
    }
    void watcherObservesOnlyInjectedFixedPathWithoutAuth()
    {
        QSignalSpy changed(backend,&AeroLoginBackend::externalChange);QSaveFile file(directory.filePath("fixed-watched-file"));QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.write("changed")>0);QVERIFY(file.commit());QTRY_VERIFY(!changed.isEmpty());QCOMPARE(runner->starts,0);
    }
};
QTEST_GUILESS_MAIN(AeroLoginBackendTest)
#include "aero_login_backend_test.moc"
