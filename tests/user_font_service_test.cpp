#include "settings/UserFontService.h"
#include "operations/OperationManager.h"
#include <QTest>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QRawFont>
#include <unistd.h>

namespace {
bool script(const QString &path, const QByteArray &body)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write("#!/bin/sh\n" + body);
    file.close();
    return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}
QString fontFixture(const QString &directory)
{
    // Read a platform font, copying into an isolated fixture before any action.
    // The real font file and real font cache are never modified.
    QDirIterator it("/usr/share/fonts", {"*.ttf", "*.otf"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (!QRawFont(path, 16).isValid()) continue;
        const QString fixture = directory + "/source.ttf";
        if (QFile::copy(path, fixture)) return fixture;
    }
    return {};
}
UserFontService::Options options(const QString &root)
{
    UserFontService::Options result;
    result.dataDirectory = root + "/data";
    result.listProgram = root + "/list";
    result.cacheProgram = root + "/cache";
    result.timeoutMs = 2000;
    script(result.cacheProgram, "exit 0\n");
    const QByteArray directory = QFile::encodeName(result.dataDirectory + "/fonts/controlpanel");
    script(result.listProgram, "for f in '" + directory + "'/*.ttf; do\n  if [ -f \"$f\" ]; then printf 'Fixture Managed\\t%s\\n' \"$f\"; fi\ndone\nexit 0\n");
    return result;
}
}
class UserFontServiceTest : public QObject {
    Q_OBJECT
private slots:
    void installRemoveAndIdentity();
    void refusesInvalidAndUnsafePaths();
    void toolFailuresAreBoundedAndTruthful();
    void cacheFailureStillRescans();
    void activityRemainsVisibleAcrossMutationStages();
};
void UserFontServiceTest::installRemoveAndIdentity()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString source = fontFixture(dir.path());
    if (source.isEmpty()) QSKIP("No readable platform font available for isolated valid-font fixture.");
    UserFontService service(options(dir.path()));
    service.installFont(source);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(service.inventoryValid());
    QCOMPARE(service.families().size(), 1);
    const QString managed = service.families().first().file;
    QVERIFY(service.canRemove(managed));
    QVERIFY(service.result().contains("Verified"));
    const QByteArray original = [&] { QFile file(managed); file.open(QIODevice::ReadOnly); return file.readAll(); }();
    service.installFont(source);
    QVERIFY(!service.busy());
    QVERIFY(service.result().contains("No file was overwritten"));
    QCOMPARE([&] { QFile file(managed); file.open(QIODevice::ReadOnly); return file.readAll(); }(), original);

    // A different inode with identical contents must not inherit authorization.
    const QString moved = dir.path() + "/saved-original.ttf";
    QVERIFY(QFile::rename(managed, moved));
    QVERIFY(QFile::copy(source, managed));
    QVERIFY(!service.canRemove(managed));
    service.removeFont(managed);
    QVERIFY(!service.busy());
    QVERIFY(QFile::exists(managed));
    QVERIFY(service.result().contains("No file was removed"));
    QVERIFY(QFile::remove(managed));
    QVERIFY(QFile::rename(moved, managed));
    QVERIFY(service.canRemove(managed));
    const QString linked = dir.path() + "/hardlink.ttf";
    QCOMPARE(::link(QFile::encodeName(managed).constData(), QFile::encodeName(linked).constData()), 0);
    QVERIFY(!service.canRemove(managed));
    service.removeFont(managed);
    QVERIFY(QFile::exists(managed));
    QVERIFY(QFile::remove(linked));
    service.removeFont(managed);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(!QFile::exists(managed));
    QVERIFY(service.families().isEmpty());
    QVERIFY(service.result().contains("Verified"));
    QVERIFY(QFile::exists(source));
}
void UserFontServiceTest::refusesInvalidAndUnsafePaths()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    auto opts = options(dir.path());
    UserFontService service(opts);
    const QString invalid = dir.path() + "/invalid.ttf";
    QFile bad(invalid); QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("not a font"); bad.close();
    service.installFont(invalid);
    QVERIFY(!service.busy());
    QVERIFY(service.result().contains("does not contain"));
    QVERIFY(!QFileInfo::exists(service.managedDirectory()));
    service.removeFont(invalid);
    QVERIFY(QFileInfo::exists(invalid));
    QVERIFY(service.result().contains("Only fonts installed"));
    const QString source = fontFixture(dir.path());
    if (source.isEmpty()) QSKIP("No valid-font fixture available.");
    const QString symlink = dir.path() + "/font-link.ttf";
    QVERIFY(QFile::link(source, symlink));
    service.installFont(symlink);
    QVERIFY(!service.busy());
    QVERIFY(service.result().contains("symbolic links"));
    QVERIFY(QDir().mkpath(opts.dataDirectory + "/fonts"));
    QVERIFY(QDir().mkpath(dir.path() + "/outside"));
    QVERIFY(QFile::link(dir.path() + "/outside", service.managedDirectory()));
    service.installFont(source);
    QVERIFY(!service.busy());
    QVERIFY(service.result().contains("unsafe"));
    QVERIFY(QDir(dir.path() + "/outside").entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}
void UserFontServiceTest::toolFailuresAreBoundedAndTruthful()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    auto opts = options(dir.path());
    QVERIFY(script(opts.listProgram, "printf 'Fixture\\t/tmp/font.ttf\\n'\n"));
    UserFontService service(opts);
    service.refresh();
    QVERIFY(service.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(service.inventoryValid()); QCOMPARE(service.families().size(), 1);
    QVERIFY(script(opts.listProgram, "printf 'partial'\nexit 7\n"));
    service.refresh(); QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(!service.inventoryValid()); QCOMPARE(service.families().size(), 1); // Retained, marked stale.
    QVERIFY(service.result().contains("Could not verify"));
    opts.outputLimit = 20;
    QVERIFY(script(opts.listProgram, "printf 'abcdefghijklmnopqrstuvwxyz0123456789'\n"));
    UserFontService bounded(opts);
    bounded.refresh(); QTRY_VERIFY_WITH_TIMEOUT(!bounded.busy(), 5000);
    QVERIFY(!bounded.inventoryValid()); QVERIFY(bounded.result().contains("too much output"));
    opts.timeoutMs = 50;
    QVERIFY(script(opts.listProgram, "exec sleep 3\n"));
    UserFontService timed(opts);
    timed.refresh(); QTRY_VERIFY_WITH_TIMEOUT(!timed.busy(), 3000);
    QVERIFY(timed.result().contains("timed out"));
    opts.listProgram = dir.path() + "/missing";
    UserFontService missing(opts);
    missing.refresh(); QTRY_VERIFY_WITH_TIMEOUT(!missing.busy(), 3000);
    QVERIFY(!missing.inventoryValid());
    QVERIFY(!OperationManager::instance()->hasActiveJobs());
}
void UserFontServiceTest::cacheFailureStillRescans()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString source = fontFixture(dir.path());
    if (source.isEmpty()) QSKIP("No valid-font fixture available.");
    auto opts = options(dir.path());
    QVERIFY(script(opts.cacheProgram, "exit 9\n"));
    UserFontService service(opts);
    service.installFont(source);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(service.inventoryValid()); QCOMPARE(service.families().size(), 1);
    QVERIFY(service.result().contains("Font cache refresh failed"));
    QVERIFY(service.result().contains("Verified"));
    QVERIFY(!OperationManager::instance()->hasActiveMutatingJobs());
}
void UserFontServiceTest::activityRemainsVisibleAcrossMutationStages()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString source = fontFixture(dir.path());
    if (source.isEmpty()) QSKIP("No valid-font fixture available.");
    auto opts = options(dir.path());
    QVERIFY(script(opts.cacheProgram, "sleep 0.15\nexit 0\n"));
    QVERIFY(script(opts.listProgram, "sleep 0.15\nprintf 'Fixture\t/tmp/fixture.ttf\n'\n"));
    UserFontService service(opts);
    service.installFont(source);
    QVERIFY(service.busy());
    QVERIFY(OperationManager::instance()->hasActiveMutatingJobs());
    QVERIFY(OperationManager::instance()->activityTitles().contains("Refreshing the user font cache"));
    QTRY_VERIFY_WITH_TIMEOUT(OperationManager::instance()->activityTitles().contains("Reading installed fonts"), 3000);
    QVERIFY(service.busy());
    QVERIFY(OperationManager::instance()->hasActiveMutatingJobs());
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 3000);
    QVERIFY(OperationManager::instance()->activityTitles().isEmpty());
    QVERIFY(!OperationManager::instance()->hasActiveMutatingJobs());
}
QTEST_MAIN(UserFontServiceTest)
#include "user_font_service_test.moc"
