#include <QtTest>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>

#include "DateTimeDialog.h"

class DateTimeDialogTest : public QObject {
    Q_OBJECT

private slots:
    void unsupportedControlsAndCloseOnly()
    {
        DateTimeDialog dialog(nullptr, [] { return TimeSync::Status{}; });
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        QCOMPARE(buttons->standardButtons(), QDialogButtonBox::StandardButtons(QDialogButtonBox::Close));
        QVERIFY(!buttons->button(QDialogButtonBox::Apply));
        QVERIFY(!buttons->button(QDialogButtonBox::Cancel));

        QCheckBox *notify = nullptr;
        for (QCheckBox *box : dialog.findChildren<QCheckBox *>())
            if (box->text() == QStringLiteral("Notify me when the clock changes"))
                notify = box;
        QVERIFY(notify);
        QVERIFY(!notify->isEnabled());
        QVERIFY(!notify->isChecked());
        QVERIFY(!notify->toolTip().isEmpty());
        for (int i = 1; i <= 2; ++i) {
            auto *clock = dialog.findChild<QGroupBox *>(QStringLiteral("clock%1_show").arg(i));
            QVERIFY(clock);
            QVERIFY(!clock->isEnabled());
            QVERIFY(!clock->isChecked());
        }
    }

    void unknownStatusDoesNotClaimOffOrSynchronized()
    {
        DateTimeDialog dialog(nullptr, [] { return TimeSync::Status{}; });
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("internetTimeStatus"));
        auto *sync = dialog.findChild<QLabel *>(QStringLiteral("internetTimeSynchronization"));
        auto *change = dialog.findChild<QPushButton *>(QStringLiteral("internetTimeChange"));
        QVERIFY(status);
        QVERIFY(sync);
        QVERIFY(change);
        QTRY_COMPARE(status->text(), QStringLiteral("Automatic time synchronization: Unknown."));
        QVERIFY(!status->text().contains(QStringLiteral("Off.")));
        QCOMPARE(sync->text(), QStringLiteral("Clock synchronization could not be verified."));
        QVERIFY(!change->isEnabled());
        QVERIFY(!change->toolTip().isEmpty());
    }

    void chronyEnabledIsDistinctFromSynchronized_data()
    {
        QTest::addColumn<bool>("synchronized");
        QTest::newRow("enabled-but-not-synchronized") << false;
        QTest::newRow("enabled-and-synchronized") << true;
    }

    void chronyEnabledIsDistinctFromSynchronized()
    {
        QFETCH(bool, synchronized);
        TimeSync::Status fixture;
        fixture.provider = TimeSync::Provider::Chrony;
        fixture.service = QStringLiteral("chrony.service");
        fixture.enabled = true;
        fixture.synchronized = synchronized;
        fixture.configuredServer = QStringLiteral("time.example.test");
        fixture.activeSource = QStringLiteral("192.0.2.123");
        DateTimeDialog dialog(nullptr, [fixture] { return fixture; });
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("internetTimeStatus"));
        auto *sync = dialog.findChild<QLabel *>(QStringLiteral("internetTimeSynchronization"));
        QVERIFY(status);
        QVERIFY(sync);
        QTRY_VERIFY(status->text().startsWith(QStringLiteral("Automatic time synchronization: On.")));
        QVERIFY(status->text().contains(QStringLiteral("Time service: Chrony.")));
        QVERIFY(status->text().contains(QStringLiteral("Preferred server: time.example.test.")));
        QVERIFY(status->text().contains(QStringLiteral("Current source: 192.0.2.123.")));
        QCOMPARE(sync->text(), synchronized
            ? QStringLiteral("The system clock reports synchronized.")
            : QStringLiteral("The system clock does not currently report synchronization."));

        // Optional fixture-only visual QA; never click privileged controls.
        const QString screenshotDir = qEnvironmentVariable("CONTROL_DATE_TIME_TEST_SCREENSHOT_DIR");
        if (!screenshotDir.isEmpty() && !synchronized) {
            QVERIFY(QDir().mkpath(screenshotDir));
            dialog.showTab(DateTimeDialog::TabInternetTime);
            dialog.show();
            QApplication::processEvents();
            QVERIFY(dialog.grab().save(QDir(screenshotDir).filePath(QStringLiteral("date-time-chrony-unsynchronized.png"))));
            dialog.showTab(DateTimeDialog::TabAdditionalClocks);
            QApplication::processEvents();
            QVERIFY(dialog.grab().save(QDir(screenshotDir).filePath(QStringLiteral("date-time-additional-clocks.png"))));
        }
    }
};

QTEST_MAIN(DateTimeDialogTest)
#include "date_time_dialog_test.moc"
