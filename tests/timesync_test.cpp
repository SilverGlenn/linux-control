#include <QtTest>

#include "time/TimeSync.h"

using namespace TimeSync;

class TimeSyncTest : public QObject {
    Q_OBJECT

private slots:
    void validServers_data()
    {
        QTest::addColumn<QString>("server");
        QTest::addColumn<bool>("valid");
        const QStringList accepted = {
            QStringLiteral("time.example.org"), QStringLiteral("localhost"),
            QStringLiteral("0.pool.ntp.org"), QStringLiteral("xn--bcher-kva.example"),
            QStringLiteral("192.0.2.42"), QStringLiteral("2001:db8::1"),
            QStringLiteral("::1"), QStringLiteral("::ffff:192.0.2.1"),
            QString(63, QLatin1Char('a')) + QStringLiteral(".example")
        };
        for (const QString &value : accepted)
            QTest::newRow(qPrintable(value)) << value << true;
        const QStringList rejected = {
            QString(), QStringLiteral(" "), QStringLiteral(" time.example.org"),
            QStringLiteral("time.example.org\nserver attacker.example"),
            QStringLiteral("time.example.org\n"), QStringLiteral("time.example.org\t"),
            QStringLiteral("--help"), QStringLiteral("-x.example"),
            QStringLiteral("example-.org"), QStringLiteral("https://time.example.org"),
            QStringLiteral("example.org:123"), QStringLiteral("[2001:db8::1]"),
            QStringLiteral("fe80::1%eth0"), QStringLiteral("a;id"),
            QStringLiteral("$(id)"), QStringLiteral("`id`"), QStringLiteral("a#comment"),
            QStringLiteral("a/b"), QStringLiteral("a\\b"), QStringLiteral("a_b.example"),
            QStringLiteral("*.example"), QStringLiteral("a..example"),
            QStringLiteral("example.org."), QStringLiteral("bücher.example"),
            QStringLiteral("999.1.1.1"), QStringLiteral("127.1"), QStringLiteral("1.2.3"),
            QStringLiteral("010.0.0.1"), QStringLiteral("1.2.3.04"),
            QStringLiteral("1.2.3.4.5"), QStringLiteral("1.2.3."), QStringLiteral("12345"),
            QString(64, QLatin1Char('a')) + QStringLiteral(".example"),
            QString(254, QLatin1Char('a'))
        };
        int row = 0;
        for (const QString &value : rejected)
            QTest::newRow(qPrintable(QStringLiteral("reject-%1").arg(++row))) << value << false;
    }

    void validServers()
    {
        QFETCH(QString, server);
        QFETCH(bool, valid);
        QCOMPARE(validServer(server), valid);
    }

    void selectedSources()
    {
        const QString csv = QStringLiteral(
            "^,+,combined.example,2,6,377,12,0.0,0.0,0.1\n"
            "^,?,unreachable.example,2,6,0,0,0.0,0.0,0.1\n"
            "^,*,selected.example,2,6,377,12,0.0,0.0,0.1\n");
        QCOMPARE(selectedChronySource(csv), QStringLiteral("selected.example"));
        QCOMPARE(selectedChronySource(QStringLiteral("=,*,192.0.2.1,2,6,377\n")),
                 QStringLiteral("192.0.2.1"));
        QCOMPARE(selectedChronySource(QStringLiteral("#,*,GPS0,0,4,377\n")),
                 QStringLiteral("Reference clock (GPS0)"));
        QVERIFY(selectedChronySource(QStringLiteral("^,+,other.example,2,6,377\n")).isEmpty());
        QVERIFY(selectedChronySource(QStringLiteral("Could not open connection to daemon\n")).isEmpty());
        QVERIFY(selectedChronySource(QStringLiteral("^* selected.example 2 6 377\n")).isEmpty());
        QVERIFY(selectedChronySource(QStringLiteral("^,*,one.example\n^,*,two.example\n")).isEmpty());
    }

    void sourcesDirectory()
    {
        const QString directory = QStringLiteral("/etc/chrony/sources.d");
        QCOMPARE(chronySourcesDirectory(QStringLiteral("sourcedir /etc/chrony/sources.d\n")), directory);
        QCOMPARE(chronySourcesDirectory(QStringLiteral(
            "# sources\n sourcedir /run/chrony-dhcp /etc/chrony/sources.d # included\n")), directory);
        QVERIFY(chronySourcesDirectory(QStringLiteral("# sourcedir /etc/chrony/sources.d\n")).isEmpty());
        QVERIFY(chronySourcesDirectory(QStringLiteral("! sourcedir /etc/chrony/sources.d\n")).isEmpty());
        QVERIFY(chronySourcesDirectory(QStringLiteral("include /etc/chrony/sources.d\n")).isEmpty());
        QVERIFY(chronySourcesDirectory(QStringLiteral("sourcedir /etc/chrony/sources.d-custom\n")).isEmpty());
        QVERIFY(chronySourcesDirectory(QStringLiteral("sourcedir /custom/sources.d\n")).isEmpty());
        QVERIFY(chronySourcesDirectory(QStringLiteral("sourcedir /etc/chrony/sources.d/\n")).isEmpty());
    }

    void ownedConfiguration()
    {
        QCOMPARE(preferredServer(QStringLiteral("# Managed file\nserver clock.example iburst prefer\n")),
                 QStringLiteral("clock.example"));
        QCOMPARE(preferredServer(QStringLiteral("[Time]\nNTP=\nNTP=2001:db8::1\n")),
                 QStringLiteral("2001:db8::1"));
        const QStringList rejected = {
            QStringLiteral("[Time]\nNTP=old.example"), // no reset; may append to previous sources
            QStringLiteral("server clock.example iburst"),
            QStringLiteral("server clock.example iburst prefer extra"),
            QStringLiteral("pool clock.example iburst prefer"),
            QStringLiteral("server clock.example iburst prefer\nserver other.example iburst prefer"),
            QStringLiteral("[Time]\nNTP=one.example two.example"),
            QStringLiteral("[Time]\nNTP=one.example\nFallbackNTP=other.example"),
            QStringLiteral("[Other]\nNTP=one.example"),
            QStringLiteral("[Time]\nNTP=--help"),
            QStringLiteral("[Time]\nNTP=one.example\n[Time]\nNTP=two.example")
        };
        for (const QString &text : rejected)
            QVERIFY2(preferredServer(text).isEmpty(), qPrintable(text));
    }

    void activeProviderWins()
    {
        const QList<ServiceProbe> probes = {
            {Provider::Chrony, QStringLiteral("chrony.service"), true, true, true},
            {Provider::Timesyncd, QStringLiteral("systemd-timesyncd.service"), true, true, false}
        };
        const Status status = statusFromProbes(probes,
            QStringLiteral("NTP=yes\nNTPSynchronized=no\n"), true);
        QCOMPARE(status.provider, Provider::Chrony);
        QCOMPARE(status.service, QStringLiteral("chrony.service"));
        QCOMPARE(status.enabled, std::optional<bool>(true));
        QCOMPARE(status.synchronized, std::optional<bool>(false));
        QVERIFY(!status.canEditServer); // Read-only selection does not assume a writable config.
        const Status partial = statusFromProbes({probes.first(),
            {Provider::Timesyncd, QStringLiteral("systemd-timesyncd.service"), false, false, false}
        }, QString(), false);
        QCOMPARE(partial.provider, Provider::Chrony);
        QVERIFY(!partial.enabled.has_value());
    }

    void probeFailureIsUnknown()
    {
        const Status status = statusFromProbes({
            {Provider::Chrony, QStringLiteral("chrony.service"), false, false, false},
            {Provider::Timesyncd, QStringLiteral("systemd-timesyncd.service"), false, false, false}
        }, QStringLiteral("NTP=no\nNTPSynchronized=no\n"), false);
        QCOMPARE(status.provider, Provider::Unknown);
        QVERIFY(!status.enabled.has_value());
        QVERIFY(!status.synchronized.has_value());
        QVERIFY(!status.detail.isEmpty());
        const Status partial = statusFromProbes({
            {Provider::Chrony, QStringLiteral("chrony.service"), true, true, false},
            {Provider::Timesyncd, QStringLiteral("systemd-timesyncd.service"), false, false, false}
        }, QString(), true);
        QCOMPARE(partial.provider, Provider::Unknown);
        QVERIFY(!partial.enabled.has_value());
    }

    void conflictsAndAliases()
    {
        const ServiceProbe chrony{Provider::Chrony, QStringLiteral("chrony.service"), true, true, true};
        const ServiceProbe timesyncd{Provider::Timesyncd, QStringLiteral("systemd-timesyncd.service"), true, true, true};
        QCOMPARE(statusFromProbes({chrony, timesyncd}, QString(), true).provider, Provider::Unknown);
        const ServiceProbe other{Provider::Unknown, QStringLiteral("ntp.service"), true, true, true};
        QCOMPARE(statusFromProbes({chrony, other}, QString(), true).provider, Provider::Unknown);
        QCOMPARE(statusFromProbes({other}, QString(), true).provider, Provider::Unknown);
        QCOMPARE(statusFromProbes({chrony, chrony}, QString(), true).provider, Provider::Chrony);
    }

    void missingUnitIsExplicitAbsence()
    {
        const QString service = QStringLiteral("systemd-timesyncd.service");
        const ServiceProbe absent = serviceProbeFromOutput(Provider::Timesyncd, service,
            QStringLiteral("Id=systemd-timesyncd.service\nLoadState=not-found\nActiveState=inactive\n"), false);
        QVERIFY(absent.querySucceeded);
        QVERIFY(!absent.installed);
        QVERIFY(!absent.active);
        const ServiceProbe busError = serviceProbeFromOutput(Provider::Timesyncd, service,
            QStringLiteral("Failed to connect to bus"), false);
        QVERIFY(!busError.querySucceeded);
        const ServiceProbe incomplete = serviceProbeFromOutput(Provider::Timesyncd, service,
            QStringLiteral("Id=systemd-timesyncd.service\nLoadState=not-found\n"), false);
        QVERIFY(!incomplete.querySucceeded);
        const ServiceProbe wrongUnit = serviceProbeFromOutput(Provider::Timesyncd, service,
            QStringLiteral("Id=other.service\nLoadState=not-found\nActiveState=inactive\n"), false);
        QVERIFY(!wrongUnit.querySucceeded);
        const ServiceProbe chrony{Provider::Chrony, QStringLiteral("chrony.service"), true, true, false};
        QCOMPARE(statusFromProbes({chrony, absent}, QStringLiteral("NTP=no\n"), true).provider,
                 Provider::Chrony);
    }

    void inconsistentEnabledState()
    {
        const ServiceProbe active{Provider::Chrony, QStringLiteral("chrony.service"), true, true, true};
        const Status activeButDisabled = statusFromProbes({active},
            QStringLiteral("NTP=no\nNTPSynchronized=yes\n"), true);
        QCOMPARE(activeButDisabled.provider, Provider::Chrony);
        QVERIFY(!activeButDisabled.enabled.has_value());
        QVERIFY(activeButDisabled.detail.contains(QStringLiteral("conflicts")));
        ServiceProbe inactive = active;
        inactive.active = false;
        const Status inactiveButEnabled = statusFromProbes({inactive},
            QStringLiteral("NTP=yes\nNTPSynchronized=no\n"), true);
        QCOMPARE(inactiveButEnabled.provider, Provider::Unknown);
        QVERIFY(inactiveButEnabled.service.isEmpty());
        QVERIFY(!inactiveButEnabled.enabled.has_value());
        QVERIFY(inactiveButEnabled.detail.contains(QStringLiteral("no active time service")));
    }

    void inactiveProviderMustBeUnambiguous()
    {
        const ServiceProbe chrony{Provider::Chrony, QStringLiteral("chrony.service"), true, true, false};
        const ServiceProbe timesyncd{Provider::Timesyncd, QStringLiteral("systemd-timesyncd.service"), true, true, false};
        QCOMPARE(statusFromProbes({chrony}, QStringLiteral("NTP=no\n"), true).provider, Provider::Chrony);
        QCOMPARE(statusFromProbes({chrony, timesyncd}, QStringLiteral("NTP=no\n"), true).provider, Provider::Unknown);
        const Status unknown = statusFromProbes({}, QStringLiteral("NTP=garbage\nNTPSynchronized=\n"), true);
        QVERIFY(!unknown.enabled.has_value());
        QVERIFY(!unknown.synchronized.has_value());
    }
};

QTEST_APPLESS_MAIN(TimeSyncTest)
#include "timesync_test.moc"
