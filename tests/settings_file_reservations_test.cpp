#include <QtTest>
#include <QtConcurrent/QtConcurrentRun>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <sys/stat.h>
#include <unistd.h>
#include "operations/SettingsFileReservations.h"
#include "operations/OperationManager.h"
namespace R=SettingsFileReservations;
class SettingsFileReservationsTest final:public QObject {
    Q_OBJECT
    QList<R::Reservation> held;
    static bool put(const QString &path,const QByteArray &bytes="private fixture") {
        if(!QDir().mkpath(QFileInfo(path).absolutePath()))return false;
        QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size();
    }
    static R::Inspection inspect(const QStringList &paths) {
        const auto inventory=R::snapshot();return QtConcurrent::run([paths,inventory]{return R::inspect(paths,inventory);}).result();
    }
    static QStringList keys(const QStringList &paths) {return QtConcurrent::run([paths]{return R::currentKeys(paths);}).result();}
    R::Reservation acquire(const R::Inspection &inspection,QString &error) {
        auto reservation=R::acquire(inspection,error);if(reservation.valid())held<<reservation;return reservation;
    }
private slots:
    void cleanup(){for(const auto &reservation:held)R::release(reservation);held.clear();QVERIFY(R::snapshot().active.isEmpty());}
    void unrelatedFilesAndPackageMutationLeaseAreIndependent(){
        QTemporaryDir dir;const auto a=dir.filePath("a"),b=dir.filePath("b");QVERIFY(put(a));QVERIFY(put(b));QString error;
        QObject packageOwner;QVERIFY(OperationManager::instance()->acquireMutation(&packageOwner));
        const auto first=acquire(inspect({a}),error);QVERIFY2(first.valid(),qPrintable(error));
        const auto second=acquire(inspect({b}),error);QVERIFY2(second.valid(),qPrintable(error));
        QVERIFY(OperationManager::instance()->hasActiveMutatingJobs());R::release(first);R::release(second);
        QVERIFY(OperationManager::instance()->hasActiveMutatingJobs());OperationManager::instance()->releaseMutation(&packageOwner);
    }
    void sharedGlobalFileRejectsCrossProviderOverlap(){
        QTemporaryDir dir;const auto a=dir.filePath("provider-a"),b=dir.filePath("provider-b"),global=dir.filePath("kdeglobals");
        QVERIFY(put(a));QVERIFY(put(b));QVERIFY(put(global));QString error;
        QVERIFY(acquire(inspect({a,global}),error).valid());QVERIFY(!acquire(inspect({b,global}),error).valid());QVERIFY(error.contains("owns"));
    }
    void canonicalAliasesAndMissingPathsShareReservations(){
        QTemporaryDir dir;const auto real=dir.filePath("real"),alias=dir.filePath("alias");QVERIFY(QDir().mkpath(real));QVERIFY(QFile::link(real,alias));QString error;
        const auto missing=real+"/nested/settings";QVERIFY(acquire(inspect({missing}),error).valid());
        QVERIFY(!acquire(inspect({alias+"/nested/settings"}),error).valid());QVERIFY(error.contains("owns"));
    }
    void replacementInodeAndOriginalHardlinkRemainHeldUntilTerminal(){
        QTemporaryDir dir;const auto path=dir.filePath("settings"),oldAlias=dir.filePath("old-link"),newAlias=dir.filePath("new-link");
        QVERIFY(put(path,"before"));QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(oldAlias).constData())==0);QString error;
        const auto owner=acquire(inspect({path}),error);QVERIFY(owner.valid());QVERIFY(R::beginMutation(owner));
        QtConcurrent::run([path]{QSaveFile file(path);if(file.open(QIODevice::WriteOnly)){file.write("replacement");file.commit();}}).waitForFinished();R::endMutation(owner);
        QVERIFY(::link(QFile::encodeName(path).constData(),QFile::encodeName(newAlias).constData())==0);
        // Admission refreshes active identities even before the GUI save result
        // has appended replacement keys to the owner's retained inventory.
        QVERIFY(!acquire(inspect({newAlias}),error).valid());QVERIFY(!acquire(inspect({oldAlias}),error).valid());
        QVERIFY(R::refresh(owner,keys({path})));QVERIFY(!acquire(inspect({newAlias}),error).valid());
        R::release(owner);QVERIFY(acquire(inspect({newAlias}),error).valid());
    }
    void staleRegistryAndMutationEpochInspectionCannotAcquire(){
        QTemporaryDir dir;const auto a=dir.filePath("a"),b=dir.filePath("b");QVERIFY(put(a));QVERIFY(put(b));QString error;
        const auto stale=inspect({b});const auto owner=acquire(inspect({a}),error);QVERIFY(owner.valid());
        QVERIFY(!acquire(stale,error).valid());QVERIFY(error.contains("Revert to reload"));
        QVERIFY(R::beginMutation(owner));QVERIFY(!R::beginMutation(owner));
        const auto inFlight=inspect({b});QVERIFY2(inFlight.success,qPrintable(inFlight.error));
        const auto disjoint=acquire(inFlight,error);QVERIFY2(disjoint.valid(),qPrintable(error));R::release(disjoint);
        const auto missing=inspect({dir.filePath("missing")});QVERIFY(!missing.success);QVERIFY(missing.error.contains("in flight"));
        const auto beforeMutation=inspect({b});QVERIFY(beforeMutation.success);R::endMutation(owner);R::endMutation(owner);QVERIFY(!acquire(beforeMutation,error).valid());QVERIFY(error.contains("mutation changed"));
        QVERIFY(acquire(inspect({b}),error).valid());
    }
    void unsafeAmbiguityAndInvalidHandlesFailWithoutRegistryChanges(){
        QTemporaryDir dir;QString error;const auto directory=dir.filePath("directory"),broken=dir.filePath("broken"),fifo=dir.filePath("fifo");
        QVERIFY(QDir().mkpath(directory));QVERIFY(QFile::link(dir.filePath("absent"),broken));QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(),0600)==0);
        for(const auto &path:QStringList{directory,broken,fifo,QStringLiteral("relative")}){const auto inspected=inspect({path});QVERIFY(!inspected.success);QVERIFY(!inspected.error.isEmpty());QVERIFY(!acquire(inspected,error).valid());}
        QVERIFY(!R::beginMutation({}));R::endMutation({});R::release({});QVERIFY(!R::refresh({},{}));QVERIFY(R::snapshot().active.isEmpty());
    }
    void refreshRetainsKeysAndInvalidatesEarlierAdmission(){
        QTemporaryDir dir;const auto a=dir.filePath("a"),b=dir.filePath("b");QVERIFY(put(a));QVERIFY(put(b));QString error;
        const auto owner=acquire(inspect({a}),error);QVERIFY(owner.valid());const auto stale=inspect({b});
        QVERIFY(R::refresh(owner,keys({a})));QVERIFY(!acquire(stale,error).valid());
        const auto generation=R::snapshot().generation;R::release(owner);R::release(owner);QCOMPARE(R::snapshot().generation,generation+1);
        QVERIFY(acquire(inspect({a}),error).valid());
    }
};
QTEST_GUILESS_MAIN(SettingsFileReservationsTest)
#include "settings_file_reservations_test.moc"
