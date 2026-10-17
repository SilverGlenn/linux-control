#include "settings/PowerProfileService.h"

#include <QtTest>

class FakePowerProfileBackend final : public PowerProfileBackend {
public:
    QStringList availableProfiles(QString *error) override
    {
        if (error)
            error->clear();
        return {QStringLiteral("balanced"), QStringLiteral("performance")};
    }

    QString activeProfile(QString *error) override
    {
        if (error)
            error->clear();
        return active;
    }

    void setActiveProfile(const QString &profile,
                          Completion completion) override
    {
        requested = profile;
        finish = std::move(completion);
    }

    void complete(const QString &error = {})
    {
        auto callback = std::move(finish);
        QVERIFY(bool(callback));
        callback(error);
    }

    QString active = QStringLiteral("balanced");
    QString requested;
    Completion finish;
};

class PowerLifecycleTest : public QObject {
    Q_OBJECT
private slots:
    void writeTracksActualValueAndRetainsFailures()
    {
        auto fake = std::make_unique<FakePowerProfileBackend>();
        FakePowerProfileBackend *backend = fake.get();
        PowerProfileService service(std::move(fake));
        QSignalSpy changes(&service, &PowerProfileService::stateChanged);

        QCOMPARE(service.activeProfile(), QStringLiteral("balanced"));
        service.setActiveProfile(QStringLiteral("performance"));
        QVERIFY(service.isBusy());
        QCOMPARE(backend->requested, QStringLiteral("performance"));
        // The requested value is never presented as active while the write is
        // pending; it continues to reflect the backend's last actual read.
        QCOMPARE(service.activeProfile(), QStringLiteral("balanced"));

        backend->active = QStringLiteral("performance");
        backend->complete();
        QVERIFY(!service.isBusy());
        QCOMPARE(service.activeProfile(), QStringLiteral("performance"));
        QVERIFY(service.error().isEmpty());

        service.setActiveProfile(QStringLiteral("balanced"));
        QVERIFY(service.isBusy());
        backend->complete(QStringLiteral("permission denied"));
        QVERIFY(!service.isBusy());
        QCOMPARE(service.activeProfile(), QStringLiteral("performance"));
        QCOMPARE(service.error(), QStringLiteral("permission denied"));

        service.setActiveProfile(QStringLiteral("balanced"));
        backend->complete(); // method reply succeeded, actual value did not move
        QCOMPARE(service.activeProfile(), QStringLiteral("performance"));
        QVERIFY(service.error().contains(QStringLiteral("kept")));

        service.refresh();
        QVERIFY(service.error().contains(QStringLiteral("kept")));
        service.dismissError();
        QVERIFY(service.error().isEmpty());
        QVERIFY(changes.size() >= 4);
    }
};

QTEST_GUILESS_MAIN(PowerLifecycleTest)
#include "power_lifecycle_test.moc"
