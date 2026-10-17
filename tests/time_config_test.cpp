#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include "time/TimeSyncConfig.h"

using namespace TimeSyncConfig;
using TimeSync::Provider;

namespace {

bool put(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray get(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

struct Fixture {
    QTemporaryDir directory;
    Paths paths;
    QList<QPair<QString, QStringList>> calls;
    Fixture()
    {
        paths.chronyConfig = directory.filePath(QStringLiteral("chrony.conf"));
        paths.chronySource = directory.filePath(QStringLiteral("90-controlpanel.sources"));
        paths.timesyncdConfig = directory.filePath(QStringLiteral("90-controlpanel.conf"));
        put(paths.chronyConfig, "sourcedir /etc/chrony/sources.d\n");
    }
    Result record(const QString &program, const QStringList &args)
    {
        calls.append({program, args});
        return {true, {}, {}};
    }
    CommandRunner runner()
    {
        return [this](const QString &program, const QStringList &args) { return record(program, args); };
    }
};

} // namespace

class TimeConfigTest : public QObject {
    Q_OBJECT
private slots:
    void maliciousInputCannotWriteOrRun_data()
    {
        QTest::addColumn<QString>("server");
        const QStringList invalid = {
            QStringLiteral("host' ; touch /tmp/unwanted ; '"), QStringLiteral("$(id)"),
            QStringLiteral("clock.example\nserver other.example"), QStringLiteral("--help"),
            QStringLiteral("clock.example iburst"), QStringLiteral("clock.example:123")
        };
        for (int i = 0; i < invalid.size(); ++i)
            QTest::newRow(qPrintable(QString::number(i))) << invalid[i];
    }

    void maliciousInputCannotWriteOrRun()
    {
        QFETCH(QString, server);
        Fixture fixture;
        const Result result = configure(Provider::Chrony, QStringLiteral("chrony.service"), true,
                                        server, fixture.paths, fixture.runner());
        QVERIFY(!result.ok);
        QVERIFY(!QFile::exists(fixture.paths.chronySource));
        QVERIFY(fixture.calls.isEmpty());
    }

    void chronyAddsPreferredSourceWithoutReplacingOtherSources()
    {
        Fixture fixture;
        const QString existing = fixture.directory.filePath(QStringLiteral("ubuntu-ntp-pools.sources"));
        const QByteArray pool = "pool ntp.ubuntu.com iburst\n";
        QVERIFY(put(existing, pool));
        const Result result = configure(Provider::Chrony, QStringLiteral("chrony.service"), true,
                                        QStringLiteral("2001:db8::1"), fixture.paths, fixture.runner());
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(get(existing), pool);
        QCOMPARE(TimeSync::preferredServer(QString::fromUtf8(get(fixture.paths.chronySource))),
                 QStringLiteral("2001:db8::1"));
        QCOMPARE(fixture.calls.size(), 2);
        QCOMPARE(fixture.calls[0].first, QStringLiteral("/usr/bin/systemctl"));
        QCOMPARE(fixture.calls[0].second, QStringList({QStringLiteral("enable"), QStringLiteral("--now"), QStringLiteral("chrony.service")}));
        QCOMPARE(fixture.calls[1].first, QStringLiteral("/usr/bin/chronyc"));
        QCOMPARE(fixture.calls[1].second, QStringList({QStringLiteral("-h"), QStringLiteral("/run/chrony/chronyd.sock"), QStringLiteral("reload"), QStringLiteral("sources")}));
    }

    void defaultSourcesOnlyRemovesManagedFile()
    {
        Fixture fixture;
        QVERIFY(put(fixture.paths.chronySource, "server old.example iburst prefer\n"));
        const QString other = fixture.directory.filePath(QStringLiteral("other.sources"));
        QVERIFY(put(other, "pool original.example iburst\n"));
        QVERIFY(configure(Provider::Chrony, QStringLiteral("chrony.service"), true,
                          {}, fixture.paths, fixture.runner()).ok);
        QVERIFY(!QFile::exists(fixture.paths.chronySource));
        QCOMPARE(get(other), QByteArray("pool original.example iburst\n"));
    }

    void disablingPreservesServerConfiguration()
    {
        Fixture fixture;
        const QByteArray original = "server old.example iburst prefer\n";
        QVERIFY(put(fixture.paths.chronySource, original));
        QVERIFY(configure(Provider::Chrony, QStringLiteral("chronyd.service"), false,
                          {}, fixture.paths, fixture.runner()).ok);
        QCOMPARE(get(fixture.paths.chronySource), original);
        QCOMPARE(fixture.calls.size(), 1);
        QCOMPARE(fixture.calls[0].second, QStringList({QStringLiteral("disable"), QStringLiteral("--now"), QStringLiteral("chronyd.service")}));
    }

    void unsupportedDirectoryAndServiceHaveNoSideEffects()
    {
        Fixture fixture;
        QVERIFY(put(fixture.paths.chronyConfig, "# sourcedir /etc/chrony/sources.d\nsourcedir /custom\n"));
        QVERIFY(!configure(Provider::Chrony, QStringLiteral("chrony.service"), true,
                           QStringLiteral("clock.example"), fixture.paths, fixture.runner()).ok);
        QVERIFY(!configure(Provider::Chrony, QStringLiteral("other.service"), false,
                           {}, fixture.paths, fixture.runner()).ok);
        QVERIFY(fixture.calls.isEmpty());
        QVERIFY(!QFile::exists(fixture.paths.chronySource));
    }

    void symlinkIsNeverFollowed()
    {
        Fixture fixture;
        const QString victim = fixture.directory.filePath(QStringLiteral("other.conf"));
        QVERIFY(put(victim, "unchanged\n"));
        QVERIFY(QFile::link(victim, fixture.paths.chronySource));
        QVERIFY(!configure(Provider::Chrony, QStringLiteral("chrony.service"), true,
                           QStringLiteral("clock.example"), fixture.paths, fixture.runner()).ok);
        QCOMPARE(get(victim), QByteArray("unchanged\n"));
        QVERIFY(fixture.calls.isEmpty());
    }

    void reloadFailureRestoresPreviousFile()
    {
        Fixture fixture;
        const QByteArray original = "server old.example iburst prefer\n";
        QVERIFY(put(fixture.paths.chronySource, original));
        int reloads = 0;
        const Result result = configure(Provider::Chrony, QStringLiteral("chrony.service"), true,
            QStringLiteral("new.example"), fixture.paths,
            [&](const QString &program, const QStringList &args) {
                fixture.record(program, args);
                if (program == QLatin1String("/usr/bin/chronyc") && ++reloads == 1)
                    return Result{false, QStringLiteral("Cannot reload sources"), {}};
                return Result{true, {}, {}};
            });
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("Cannot reload sources")));
        QCOMPARE(get(fixture.paths.chronySource), original);
        QCOMPARE(reloads, 2);
    }

    void timesyncdUsesAtomicDropInAndFixedService()
    {
        Fixture fixture;
        QVERIFY(configure(Provider::Timesyncd, QStringLiteral("systemd-timesyncd.service"), true,
                          QStringLiteral("clock.example"), fixture.paths, fixture.runner()).ok);
        QCOMPARE(TimeSync::preferredServer(QString::fromUtf8(get(fixture.paths.timesyncdConfig))),
                 QStringLiteral("clock.example"));
        QVERIFY(get(fixture.paths.timesyncdConfig).contains("NTP=\nNTP=clock.example\n"));
        QCOMPARE(fixture.calls.size(), 2);
        QCOMPARE(fixture.calls[1].second, QStringList({QStringLiteral("restart"), QStringLiteral("systemd-timesyncd.service")}));
    }

    void reloadFailureRestoresPreviouslyDisabledService()
    {
        Fixture fixture;
        const Result result = configure(Provider::Chrony, QStringLiteral("chrony.service"), true,
            QStringLiteral("new.example"), fixture.paths,
            [&](const QString &program, const QStringList &args) {
                fixture.record(program, args);
                if (program == QLatin1String("/usr/bin/chronyc"))
                    return Result{false, QStringLiteral("Cannot reload sources"), {}};
                return Result{true, {}, {}};
            }, ServiceState{false, QStringLiteral("disabled")});
        QVERIFY(!result.ok);
        QVERIFY(!QFile::exists(fixture.paths.chronySource));
        QCOMPARE(fixture.calls.size(), 4);
        QCOMPARE(fixture.calls[2].second, QStringList({QStringLiteral("stop"), QStringLiteral("chrony.service")}));
        QCOMPARE(fixture.calls[3].second, QStringList({QStringLiteral("disable"), QStringLiteral("chrony.service")}));
        QVERIFY(result.error.contains(QStringLiteral("service state were restored")));
    }

    void refreshRequestsSamplesWithoutClockStep()
    {
        Fixture fixture;
        QVERIFY(refresh(Provider::Chrony, QStringLiteral("chrony.service"), fixture.runner()).ok);
        QCOMPARE(fixture.calls[0].second, QStringList({QStringLiteral("-h"), QStringLiteral("/run/chrony/chronyd.sock"), QStringLiteral("burst"), QStringLiteral("4/4")}));
        QVERIFY(!refresh(Provider::Unknown, QStringLiteral("chrony.service"), fixture.runner()).ok);
        QCOMPARE(fixture.calls.size(), 1);
    }

    void unrecognizedExistingConfigurationIsPreserved()
    {
        Fixture fixture;
        const QByteArray custom = "pool administrator.example iburst\n";
        QVERIFY(put(fixture.paths.chronySource, custom));
        QVERIFY(!configure(Provider::Chrony, QStringLiteral("chrony.service"), true,
                           QStringLiteral("clock.example"), fixture.paths, fixture.runner()).ok);
        QCOMPARE(get(fixture.paths.chronySource), custom);
        QVERIFY(fixture.calls.isEmpty());
    }

    void writableInstallationCannotBeTrusted()
    {
        Fixture fixture;
        QVERIFY(!trustedPath(fixture.paths.chronyConfig)); // /tmp is writable
        QVERIFY(!trustedPath(QStringLiteral("relative/path")));
        QVERIFY(!trustedPath(QStringLiteral("/usr/../tmp/example"), true));
    }

    void helperRejectsMalformedArgumentsBeforeAuthorization()
    {
        const QList<QStringList> requests = {
            {}, {QStringLiteral("--refresh")},
            {QStringLiteral("--refresh"), QStringLiteral("other")},
            {QStringLiteral("--configure"), QStringLiteral("chrony"), QStringLiteral("on"), QStringLiteral("$(id)")},
            {QStringLiteral("--configure"), QStringLiteral("chrony"), QStringLiteral("maybe"), QStringLiteral("clock.example")}
        };
        for (const QStringList &request : requests) {
            QProcess process;
            process.start(QString::fromLocal8Bit(CONTROL_TIME_HELPER_TEST_PATH), request);
            QVERIFY(process.waitForFinished(3000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(process.exitCode(), 2);
        }
    }
};

QTEST_APPLESS_MAIN(TimeConfigTest)
#include "time_config_test.moc"
