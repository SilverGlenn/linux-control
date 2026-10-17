#include <QtTest>
#include <KConfig>
#include <KConfigGroup>
#include <QSaveFile>
#include <QTemporaryDir>
#include <algorithm>
#include "platform/kde/PowerPolicyBackend.h"
#include "settings/PowerPolicyFields.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QDir>

class FakePowerDaemon final : public PowerPolicyDaemon {
public:
    FakePowerDaemon()
    {
        caps.serviceAvailable = caps.schemaSupported = true;
        caps.displaySupported = caps.sleepSupported = true;
        caps.suspendActionSupported = caps.canSuspendKnown = caps.canSuspend = true;
        caps.hibernateKnown = caps.hibernateSupported = caps.hybridSuspendSupported = caps.suspendThenHibernateSupported = true;
        caps.dimSupported = caps.brightnessSupported = caps.keyboardBrightnessSupported = caps.buttonSupported = true;
        caps.lidKnown = caps.hasLid = true; caps.owner = QStringLiteral(":fixture.17"); caps.revision = QStringLiteral("fixture-caps-v1");
        caps.batteryKnown = caps.hasBattery = true;
        caps.mobileKnown = caps.defaultMobileKnown = caps.virtualMachineKnown = true;
    }
    void queryCapabilities() override { ++reads; if (!holdCapabilities) emit capabilitiesFinished(caps); }
    void reload() override { ++reloads; }
    void cancel() override { ++cancellations; emit reloadFinished(false, true, QStringLiteral("Cancelled")); }
    void acknowledge(bool success = true) { emit reloadFinished(success, false, success ? QString() : QStringLiteral("Reload denied")); }
    PowerPolicyCapabilities caps;
    bool holdCapabilities = false;
    int reads = 0, reloads = 0, cancellations = 0;
};

class PowerPolicyBackendTest : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        dir = std::make_unique<QTemporaryDir>();
        QVERIFY(dir->isValid());
        file = dir->filePath(QStringLiteral("powerdevilrc"));
        QFile output(file);
        QVERIFY(output.open(QIODevice::WriteOnly));
        output.write("[AC][Display]\nTurnOffDisplayWhenIdle=true\nTurnOffDisplayIdleTimeoutSec=600\nDisplayBrightness=77\nLockBeforeTurnOffDisplay=true\n\n"
                     "[AC][SuspendAndShutdown]\nAutoSuspendAction=1\nAutoSuspendIdleTimeoutSec=900\nLidAction=2\nSleepMode=1\n\n"
                     "[Battery][Display]\nTurnOffDisplayIdleTimeoutSec=300\n\n"
                     "[LowBattery][Performance]\nPowerProfile=power-saver\n\n"
                     "[Activities][test]\nInhibitSuspend=true\n\n"
                     "[Other]\nUnrelated=keep me\n");
        output.close();
        daemon = new FakePowerDaemon;
        backend = std::make_unique<KdePowerPolicyBackend>(file, daemon);
    }
    void cleanup() { backend.reset(); dir.reset(); }
    void readsNestedSchemaInSecondsAndProfileDefaults()
    {
        const auto current = read();
        QCOMPARE(current.profiles.size(), 3);
        QCOMPARE(current.profiles[0].displaySeconds, 600);
        QCOMPARE(current.profiles[0].suspendAction, 1u);
        QCOMPARE(current.profiles[1].displaySeconds, 300);
        QCOMPARE(current.profiles[1].suspendSeconds, 600);
        QCOMPARE(current.profiles[2].displaySeconds, 120);
        QCOMPARE(current.profiles[2].suspendSeconds, 300);
        QVERIFY(current.profiles[2].sleepEditable);
        daemon->caps.mobile = true;
        const auto mobile = read();
        QCOMPARE(mobile.profiles[1].displaySeconds, 300); // Explicit override survives.
        QCOMPARE(mobile.profiles[2].displaySeconds, 60);
        QCOMPARE(mobile.profiles[1].suspendSeconds, 300);
    }
    void savesOnlyOwnedChangesAndRequiresReloadAcknowledgement()
    {
        auto before = read();
        auto requested = before;
        requested.profiles[0].displaySeconds = 1200;
        requested.profiles[1].suspendSeconds = 1800;
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        explicitChanges(before, requested); backend->writePolicy(before, requested);
        QCOMPARE(daemon->reloads, 1);
        QVERIFY(finished.isEmpty());
        {
            KConfig config(file, KConfig::SimpleConfig);
            const auto ac = config.group("AC");
            QCOMPARE(ac.group("Display").readEntry("TurnOffDisplayIdleTimeoutSec", 0), 1200);
            QCOMPARE(ac.group("Display").readEntry("DisplayBrightness", 0), 77);
            QVERIFY(ac.group("Display").readEntry("LockBeforeTurnOffDisplay", false));
            QCOMPARE(ac.group("SuspendAndShutdown").readEntry("LidAction", 0), 2);
            QCOMPARE(ac.group("SuspendAndShutdown").readEntry("SleepMode", 0), 1);
            QCOMPARE(config.group("LowBattery").group("Performance").readEntry("PowerProfile", QString()), QStringLiteral("power-saver"));
            QVERIFY(config.group("Activities").group("test").readEntry("InhibitSuspend", false));
            QCOMPARE(config.group("Other").readEntry("Unrelated", QString()), QStringLiteral("keep me"));
            QVERIFY(!config.group("LowBattery").group("SuspendAndShutdown").hasKey("AutoSuspendAction"));
        }
        daemon->acknowledge();
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.first()[0].toBool());
        QVERIFY(finished.first()[1].toBool());
        QCOMPARE(read().profiles[0].displaySeconds, 1200);
    }
    void rejectsChangedFileAtWriteBoundaryAndPreservesExternalUnrelatedEdits()
    {
        auto before = read();
        auto requested = before;
        requested.profiles[0].displaySeconds = 1200;
        {
            KConfig config(file, KConfig::SimpleConfig);
            config.group("AC").group("Display").writeEntry("TurnOffDisplayIdleTimeoutSec", 1800);
            QVERIFY(config.sync());
        }
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        explicitChanges(before, requested); backend->writePolicy(before, requested);
        QCOMPARE(daemon->reloads, 0);
        QVERIFY(!finished.first()[0].toBool());
        before = read();
        requested = before;
        requested.profiles[0].displaySeconds = 1200;
        {
            KConfig config(file, KConfig::SimpleConfig);
            config.group("Other").writeEntry("Unrelated", "new external value");
            QVERIFY(config.sync());
        }
        explicitChanges(before, requested); backend->writePolicy(before, requested);
        QCOMPARE(daemon->reloads, 1);
        daemon->acknowledge();
        KConfig config(file, KConfig::SimpleConfig);
        QCOMPARE(config.group("Other").readEntry("Unrelated", QString()), QStringLiteral("new external value"));
    }
    void missingHardwareDisablesButUnknownDefaultsAllowExplicitChoices()
    {
        daemon->caps.hasBattery = false;
        daemon->caps.displaySupported = false;
        daemon->caps.sleepSupported = daemon->caps.suspendActionSupported = false;
        auto current = read();
        QVERIFY(!current.profiles[0].displayEditable);
        QVERIFY(!current.profiles[0].sleepEditable);
        QVERIFY(current.profiles[1].displayReason.contains("battery"));
        daemon->caps.hasBattery = true;
        daemon->caps.displaySupported = daemon->caps.sleepSupported = daemon->caps.suspendActionSupported = true;
        daemon->caps.mobileKnown = daemon->caps.virtualMachineKnown = false;
        current = read();
        QVERIFY(current.profiles[0].displayEditable); // Explicit AC value.
        QVERIFY(current.profiles[0].sleepEditable); // Explicit AC values.
        QVERIFY(current.profiles[1].displayEditable); // Explicit battery timeout.
        QVERIFY(current.profiles[1].sleepEditable);
        QVERIFY(!current.profiles[1].keys.value(PowerPolicyKey::IdleAction).valueKnown);
        QVERIFY(current.profiles[2].displayEditable);
        QVERIFY(!current.profiles[2].keys.value(PowerPolicyKey::DisplaySeconds).valueKnown);
        QVERIFY(current.profiles[1].keys.value(PowerPolicyKey::IdleAction).writable);
    }
    void absentAcTimeoutsHaveUnambiguousDefaults()
    {
        {
            KConfig config(file, KConfig::SimpleConfig);
            config.group("AC").group("Display").deleteEntry("TurnOffDisplayIdleTimeoutSec");
            config.group("AC").group("SuspendAndShutdown").deleteEntry("AutoSuspendIdleTimeoutSec");
            QVERIFY(config.sync());
        }
        daemon->caps.mobileKnown = false;
        const auto current = read();
        QVERIFY(current.profiles[0].displayEditable);
        QVERIFY(current.profiles[0].sleepEditable);
        QCOMPARE(current.profiles[0].displaySeconds, 600);
        QCOMPARE(current.profiles[0].suspendSeconds, 900);
    }
    void vmDefaultNeverAndOtherIdleActionsArePreserved()
    {
        daemon->caps.virtualMachine = true;
        auto current = read();
        QCOMPARE(current.profiles[1].suspendAction, 0u);
        {
            KConfig config(file, KConfig::SimpleConfig);
            config.group("AC").group("SuspendAndShutdown").writeEntry("AutoSuspendAction", 2u);
            QVERIFY(config.sync());
        }
        current = read();
        QVERIFY(current.profiles[0].sleepEditable); // Native Hibernate remains distinct and can be explicitly replaced.
        QVERIFY(current.profiles[0].sleepReason.isEmpty());
        auto requested = current;
        requested.profiles[0].displaySeconds = 1200;
        explicitChanges(current, requested); backend->writePolicy(current, requested);
        daemon->acknowledge();
        KConfig config(file, KConfig::SimpleConfig);
        QCOMPARE(config.group("AC").group("SuspendAndShutdown").readEntry("AutoSuspendAction", 0u), 2u);
    }
    void immutableAndUnknownSchemaCannotWrite()
    {
        QFile output(file);
        QVERIFY(output.open(QIODevice::Append));
        output.write("\n[Battery][SuspendAndShutdown]\nAutoSuspendAction[$i]=1\nAutoSuspendIdleTimeoutSec=600\n");
        output.close();
        auto current = read();
        QVERIFY(!current.profiles[1].sleepEditable);
        QVERIFY(current.profiles[1].sleepReason.contains("locked"));
        daemon->caps.schemaSupported = false;
        current = read();
        QVERIFY(!current.serviceAvailable);
        QVERIFY(!current.profiles[0].displayEditable);
        auto requested = current;
        requested.profiles[0].displaySeconds = 1200;
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        explicitChanges(current, requested); backend->writePolicy(current, requested);
        QCOMPARE(daemon->reloads, 0);
        QVERIFY(!finished.first()[0].toBool());
    }
    void outOfRangeRequestCannotSaveAnyProfile()
    {
        const auto current = read();
        auto requested = current;
        requested.profiles[0].displaySeconds = 1200;
        requested.profiles[2].suspendSeconds = 604801;
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        explicitChanges(current, requested); backend->writePolicy(current, requested);
        QVERIFY(!finished.first()[0].toBool());
        QCOMPARE(read().profiles[0].displaySeconds, 600);
        QCOMPARE(daemon->reloads, 0);
    }
    void cancellationReportsSavedButUnacknowledged()
    {
        const auto before = read();
        auto requested = before;
        requested.profiles[0].displayEnabled = false;
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        explicitChanges(before, requested); backend->writePolicy(before, requested);
        backend->cancel();
        QCOMPARE(daemon->cancellations, 1);
        QVERIFY(finished.first()[0].toBool());
        QVERIFY(!finished.first()[1].toBool());
        QVERIFY(finished.first()[2].toBool());
        QVERIFY(!read().profiles[0].displayEnabled);
    }
    void watcherSurvivesTwoAtomicReplacements()
    {
        QSignalSpy changed(backend.get(), &PowerPolicyBackend::externalChange);
        for (int seconds : {1200, 1800}) {
            QSaveFile replacement(file);
            QVERIFY(replacement.open(QIODevice::WriteOnly));
            replacement.write(QStringLiteral("[AC][Display]\nTurnOffDisplayIdleTimeoutSec=%1\n").arg(seconds).toUtf8());
            QVERIFY(replacement.commit());
            QTRY_VERIFY(!changed.isEmpty());
            changed.clear();
            QCOMPARE(read().profiles[0].displaySeconds, seconds);
        }
    }
    void nativeExtendedDefaultsAndActionCatalogsAreExact()
    {
        const auto current = read(); const auto &ac = current.profiles[0], &battery = current.profiles[1], &low = current.profiles[2];
        QVERIFY(ac.dimEnabled); QCOMPARE(ac.dimSeconds, 300); QCOMPARE(battery.dimSeconds, 120); QCOMPARE(low.dimSeconds, 60);
        QVERIFY(!ac.brightnessEnabled && !battery.brightnessEnabled && low.brightnessEnabled);
        QCOMPARE(ac.brightnessPercent, 77); QCOMPARE(battery.brightnessPercent, 70); QCOMPARE(low.brightnessPercent, 30);
        QVERIFY(!ac.keyboardBrightnessEnabled); QCOMPARE(ac.keyboardBrightnessPercent, 50);
        QCOMPARE(ac.lockedDisplaySeconds, 60); QCOMPARE(ac.powerButtonAction, 16u); QCOMPARE(ac.powerDownAction, 16u);
        QCOMPARE(battery.lidAction, 1u); QVERIFY(ac.inhibitLidActionWhenExternalMonitorPresent);
        QVERIFY(has(ac.idleActions, 0) && has(ac.idleActions, 1) && has(ac.idleActions, 2) && has(ac.idleActions, 8));
        QVERIFY(!has(ac.idleActions, 32)); QVERIFY(has(ac.powerButtonActions, 16)); QVERIFY(!has(ac.lidActions, 16));
        QVERIFY(has(ac.sleepModes, 1) && has(ac.sleepModes, 2) && has(ac.sleepModes, 3));
        QCOMPARE(current.batteryLowLevel, 10); QCOMPARE(current.batteryCriticalLevel, 5); QCOMPARE(current.batteryCriticalAction, 2u);
        QVERIFY(current.batteryLevelsEditable && current.batteryCriticalActionEditable);
        daemon->caps.mobile = true; daemon->caps.virtualMachine = true;
        auto mobile = read(); QCOMPARE(mobile.profiles[0].powerButtonAction, 128u); QCOMPARE(mobile.profiles[1].dimSeconds, 60);
        QCOMPARE(mobile.profiles[2].dimSeconds, 30); QCOMPARE(mobile.profiles[1].lidAction, 0u);
        daemon->caps.hibernateSupported = false; QCOMPARE(read().batteryCriticalAction, 1u);
        daemon->caps.canSuspend = false; QCOMPARE(read().batteryCriticalAction, 0u);
    }
    void allExtendedPoliciesRoundTripWithoutTouchingOtherNativeSettings()
    {
        const auto before = read(); auto requested = before; auto &p = requested.profiles[0];
        p.dimSeconds = 75; p.brightnessEnabled = true; p.brightnessPercent = 55;
        p.keyboardBrightnessEnabled = true; p.keyboardBrightnessPercent = 15;
        p.lockedDisplaySeconds = -2; p.lockBeforeDisplayOff = false;
        p.suspendAction = 2; p.powerButtonAction = 32; p.powerDownAction = 8; p.lidAction = 64;
        p.inhibitLidActionWhenExternalMonitorPresent = false; p.sleepMode = 3;
        requested.batteryLowLevel = 25; requested.batteryCriticalLevel = 25; requested.batteryCriticalAction = 8;
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished); explicitChanges(before, requested); backend->writePolicy(before, requested);
        QCOMPARE(daemon->reloads, 1); QVERIFY(finished.isEmpty()); daemon->acknowledge();
        QVERIFY(finished.first()[0].toBool() && finished.first()[1].toBool());
        QString verificationError;
        QVERIFY2(PowerPolicyFields::matchesRequest(before, requested, read(), verificationError), qPrintable(verificationError));
        KConfig config(file, KConfig::SimpleConfig); const auto ac = config.group("AC");
        QCOMPARE(ac.group("Display").readEntry("DimDisplayIdleTimeoutSec", 0), 75);
        QCOMPARE(ac.group("Keyboard").readEntry("KeyboardBrightness", 0), 15);
        QCOMPARE(ac.group("SuspendAndShutdown").readEntry("AutoSuspendIdleTimeoutSec", 0), 900);
        QCOMPARE(config.group("BatteryManagement").readEntry("BatteryCriticalLevel", 0), 25);
        QCOMPARE(config.group("Other").readEntry("Unrelated", QString()), QStringLiteral("keep me"));
        QVERIFY(config.group("Activities").group("test").readEntry("InhibitSuspend", false));
        QVERIFY(!config.group("BatteryManagement").hasKey("PeripheralBatteryLowLevel"));
    }
    void absentLidAndUnsupportedActionsStayRawWithoutBlockingOtherControls()
    {
        daemon->caps.hasLid = false; daemon->caps.hibernateSupported = false; daemon->caps.hybridSuspendSupported = false;
        daemon->caps.brightnessSupported = false; auto current = read();
        QVERIFY(!current.profiles[0].lidEditable && !current.profiles[0].lidExternalMonitorEditable);
        QVERIFY(!current.profiles[0].brightnessEditable); QCOMPARE(current.profiles[0].lidAction, 2u);
        QVERIFY(!has(current.profiles[0].powerButtonActions, 2)); QVERIFY(!has(current.profiles[0].sleepModes, 2));
        auto requested = current; requested.profiles[0].dimSeconds = 80;
        explicitChanges(current, requested); backend->writePolicy(current, requested); QCOMPARE(daemon->reloads, 1); daemon->acknowledge();
        const auto after = read(); QCOMPARE(after.profiles[0].lidAction, 2u); QCOMPARE(after.profiles[0].dimSeconds, 80);
    }
    void extendedRequestValidationIsAtomic_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("dim-too-short") << 0; QTest::newRow("brightness-percent") << 1; QTest::newRow("keyboard-percent") << 2;
        QTest::newRow("locked-invalid-sentinel") << 3; QTest::newRow("locked-too-short") << 4; QTest::newRow("unknown-button") << 5;
        QTest::newRow("unsupported-lid-action") << 6; QTest::newRow("invalid-sleep-mode") << 7;
        QTest::newRow("critical-exceeds-low") << 8; QTest::newRow("negative-threshold") << 9; QTest::newRow("forged-capabilities") << 10;
    }
    void extendedRequestValidationIsAtomic()
    {
        QFETCH(int, fault); const auto before = read(); auto requested = before; requested.profiles[1].displaySeconds = 1200;
        auto &p = requested.profiles[0];
        if (fault == 0) p.dimSeconds = 9;
        if (fault == 1) p.brightnessPercent = 101;
        if (fault == 2) p.keyboardBrightnessPercent = -1;
        if (fault == 3) p.lockedDisplaySeconds = -1;
        if (fault == 4) p.lockedDisplaySeconds = 5;
        if (fault == 5) p.powerButtonAction = 4;
        if (fault == 6) p.lidAction = 16;
        if (fault == 7) p.sleepMode = 4;
        if (fault == 8) requested.batteryCriticalLevel = 11;
        if (fault == 9) requested.batteryLowLevel = -1;
        if (fault == 10) p.powerButtonActions << PowerPolicyAction{4, QStringLiteral("Injected")};
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished); explicitChanges(before, requested); backend->writePolicy(before, requested);
        QCOMPARE(finished.size(), 1); QVERIFY(!finished.first()[0].toBool()); QCOMPARE(daemon->reloads, 0); QVERIFY(read() == before);
    }
    void ownerAndCapabilityDriftAtNativeWriteBoundaryRefusesAllWrites()
    {
        const auto before = read(); auto requested = before; requested.profiles[0].dimSeconds = 90;
        daemon->caps.owner = QStringLiteral(":fixture.replaced"); QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        explicitChanges(before, requested); backend->writePolicy(before, requested); QCOMPARE(finished.size(), 1); QVERIFY(!finished.first()[0].toBool()); QCOMPARE(daemon->reloads, 0);
        QCOMPARE(read().profiles[0].dimSeconds, before.profiles[0].dimSeconds);
        daemon->caps.owner = before.owner; daemon->caps.revision = QStringLiteral("changed-native-capabilities");
        explicitChanges(before, requested); backend->writePolicy(before, requested); QCOMPARE(finished.size(), 2); QVERIFY(!finished.last()[0].toBool()); QCOMPARE(daemon->reloads, 0);
    }
    void policyLocksAndUnknownCriticalDefaultAffectOnlyTheirOwnControls()
    {
        QFile output(file); QVERIFY(output.open(QIODevice::Append));
        output.write("\n[AC][Display]\nDimDisplayIdleTimeoutSec[$i]=300\n\n[BatteryManagement]\nBatteryLowLevel[$i]=10\n"); output.close();
        daemon->caps.hibernateKnown = false; const auto current = read();
        QVERIFY(!current.profiles[0].dimEditable); QVERIFY(current.profiles[0].keyboardBrightnessEditable);
        QVERIFY(!current.batteryLevelsEditable); QVERIFY(current.batteryCriticalActionEditable);
        QVERIFY(!current.keys.value(PowerPolicyKey::BatteryCriticalAction).valueKnown);
        auto requested = current; requested.profiles[0].keyboardBrightnessEnabled = true; requested.profiles[0].keyboardBrightnessPercent = 0;
        explicitChanges(current, requested); backend->writePolicy(current, requested); QCOMPARE(daemon->reloads, 1); daemon->acknowledge();
        QCOMPARE(read().profiles[0].keyboardBrightnessPercent, 0);
    }
    void cancellingNewCapabilityPreflightCannotWriteAfterItsLateReply()
    {
        const auto before = read(); auto requested = before; requested.profiles[0].dimSeconds = 90;
        daemon->holdCapabilities = true; QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        explicitChanges(before, requested); backend->writePolicy(before, requested); QVERIFY(finished.isEmpty()); backend->cancel();
        QCOMPARE(finished.size(), 1); QVERIFY(!finished.first()[0].toBool()); QVERIFY(finished.first()[2].toBool());
        emit daemon->capabilitiesFinished(daemon->caps); QCOMPARE(finished.size(), 1); QCOMPARE(daemon->reloads, 0);
        daemon->holdCapabilities = false; QVERIFY(read() == before);
    }
    void unknownEffectiveDefaultCanBeExplicitlyPinnedWithoutFlatteningCompanions()
    {
        daemon->caps.mobileKnown = false;
        auto before = read(); auto request = before; QString error;
        const auto key = PowerPolicyKey::DisplaySeconds;
        QVERIFY(!before.profiles[2].keys.value(key).valueKnown);
        QVERIFY(before.profiles[2].keys.value(key).defaultKnown);
        QVERIFY(before.profiles[2].keys.value(key).writable);
        QVERIFY(PowerPolicyFields::stage(request.profiles[2], before.profiles[2], key,
                                        QVariant(before.profiles[2].displaySeconds), error));
        QVERIFY(request.profiles[2].intents.contains(key));
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        backend->writePolicy(before, request); QCOMPARE(daemon->reloads, 1); daemon->acknowledge();
        QVERIFY(finished.first()[1].toBool());
        KConfig config(file, KConfig::SimpleConfig); auto display = config.group("LowBattery").group("Display");
        QVERIFY(display.hasKey("TurnOffDisplayIdleTimeoutSec"));
        QVERIFY(!display.hasKey("TurnOffDisplayWhenIdle"));
        QVERIFY(!display.hasKey("DimDisplayIdleTimeoutSec"));
        QVERIFY(read().profiles[2].keys.value(key).valueKnown);
    }
    void companionToggleDoesNotPersistAnUnknownTimeout()
    {
        daemon->caps.mobileKnown = false;
        auto before = read(); auto request = before; QString error;
        QVERIFY(PowerPolicyFields::stage(request.profiles[2], before.profiles[2], PowerPolicyKey::DisplayEnabled, QVariant(false), error));
        backend->writePolicy(before, request); QCOMPARE(daemon->reloads, 1); daemon->acknowledge();
        KConfig config(file, KConfig::SimpleConfig); const auto group = config.group("LowBattery").group("Display");
        QVERIFY(group.hasKey("TurnOffDisplayWhenIdle")); QVERIFY(!group.hasKey("TurnOffDisplayIdleTimeoutSec"));
        QVERIFY(!read().profiles[2].keys.value(PowerPolicyKey::DisplaySeconds).valueKnown);
    }
    void rawEditsWithoutIntentAndForgedIntentMetadataAreRejected()
    {
        const auto before = read(); auto request = before; request.profiles[0].displaySeconds = 1200;
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        backend->writePolicy(before, request); QCOMPARE(finished.size(), 1); QVERIFY(!finished.last()[0].toBool());
        explicitChanges(before, request); request.profiles[0].keys[PowerPolicyKey::DisplaySeconds].localPresent = false;
        backend->writePolicy(before, request); QCOMPARE(finished.size(), 2); QVERIFY(!finished.last()[0].toBool());
        QCOMPARE(daemon->reloads, 0); QVERIFY(read() == before);
    }
    void explicitEqualOverrideIsANativeDefaultsNoop()
    {
        const auto before = read(); auto request = before; QString error;
        const auto key = PowerPolicyKey::DisplaySeconds;
        QVERIFY(before.profiles[0].keys.value(key).localPresent);
        QVERIFY(before.profiles[0].keys.value(key).defaultKnown);
        QVERIFY(!before.profiles[0].keys.value(key).resettable);
        QVERIFY(!PowerPolicyFields::restore(request.profiles[0], before.profiles[0], key, error));
        QVERIFY(request.profiles[0].intents.isEmpty());
        QCOMPARE(request.profiles[0].keys.value(key).revision, before.profiles[0].keys.value(key).revision);
    }
    void nativeResetCanRestoreAValueOutsideExplicitSetChoices()
    {
        daemon->caps.defaultMobile = true;
        { KConfig config(file, KConfig::SimpleConfig); config.group("AC").group("SuspendAndShutdown").writeEntry("PowerButtonAction", 8u); QVERIFY(config.sync()); }
        const auto before = read(); auto request = before; QString error; const auto key = PowerPolicyKey::PowerButtonAction;
        QCOMPARE(before.profiles[0].keys.value(key).defaultValue.toUInt(), 128u);
        QVERIFY(!has(before.profiles[0].powerButtonActions, 128u));
        QVERIFY(PowerPolicyFields::restore(request.profiles[0], before.profiles[0], key, error));
        backend->writePolicy(before, request); QCOMPARE(daemon->reloads, 1); daemon->acknowledge();
        KConfig config(file, KConfig::SimpleConfig);
        QVERIFY(!config.group("AC").group("SuspendAndShutdown").hasKey("PowerButtonAction"));
        // The daemon has independent nonmobile defaults, so the effective value
        // after the native KCM removal is 16, rather than editor default 128.
        QCOMPARE(read().profiles[0].powerButtonAction, 16u);
    }
    void resetRemovalCanLeaveEffectiveDefaultUnverified()
    {
        daemon->caps.mobileKnown = false;
        { KConfig config(file, KConfig::SimpleConfig); config.group("LowBattery").group("Display").writeEntry("TurnOffDisplayIdleTimeoutSec", 600); QVERIFY(config.sync()); }
        const auto before = read(); auto request = before; QString error; const auto key = PowerPolicyKey::DisplaySeconds;
        QVERIFY(before.profiles[2].keys.value(key).valueKnown);
        QVERIFY(PowerPolicyFields::restore(request.profiles[2], before.profiles[2], key, error));
        backend->writePolicy(before, request); QCOMPARE(daemon->reloads, 1); daemon->acknowledge();
        const auto after = read(); QVERIFY(!after.profiles[2].keys.value(key).valueKnown);
        QVERIFY(!after.profiles[2].keys.value(key).effectivePresent);
        QVERIFY(!after.profiles[2].keys.value(key).resettable);
    }
    void rawEquivalentExternalEditStillInvalidatesRevision()
    {
        const auto before = read(); auto request = before; QString error;
        QVERIFY(PowerPolicyFields::stage(request.profiles[0], before.profiles[0], PowerPolicyKey::DisplaySeconds, QVariant(1200), error));
        { QFile output(file); QVERIFY(output.open(QIODevice::Append)); output.write("\n[AC][Display]\nTurnOffDisplayIdleTimeoutSec=0600\n"); }
        QCOMPARE(read().profiles[0].displaySeconds, before.profiles[0].displaySeconds);
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        backend->writePolicy(before, request); QCOMPARE(finished.size(), 1); QVERIFY(!finished.first()[0].toBool()); QCOMPARE(daemon->reloads, 0);
    }
    void nativeCustomTimeoutMaximumRoundTripsWithoutMillisecondsOverflow()
    {
        const auto before=read(); auto request=before; QString error;
        for(auto key:{PowerPolicyKey::DisplaySeconds,PowerPolicyKey::IdleSeconds,PowerPolicyKey::DimSeconds,PowerPolicyKey::LockedDisplaySeconds})
            QVERIFY(PowerPolicyFields::stage(request.profiles[0],before.profiles[0],key,604800,error));
        backend->writePolicy(before,request); QCOMPARE(daemon->reloads,1); daemon->acknowledge();
        QVERIFY2(PowerPolicyFields::matchesRequest(before,request,read(),error),qPrintable(error));
        KConfig config(file,KConfig::SimpleConfig);
        QCOMPARE(config.group("AC").group("Display").readEntry("TurnOffDisplayIdleTimeoutSec",0),604800);
        QCOMPARE(config.group("AC").group("SuspendAndShutdown").readEntry("AutoSuspendIdleTimeoutSec",0),604800);
    }
    void fullCascadeAndGlobalOriginsRunOnlyInAnIsolatedChild()
    {
        if (qEnvironmentVariableIsSet("CONTROL_POWER_CASCADE_CHILD")) QSKIP("Parent-only launcher.");
        QTemporaryDir sandbox; QVERIFY(sandbox.isValid());
        const auto home = sandbox.filePath("user"), system = sandbox.filePath("system");
        QVERIFY(QDir().mkpath(home)); QVERIFY(QDir().mkpath(system));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_CONFIG_HOME", home); environment.insert("XDG_CONFIG_DIRS", system);
        environment.insert("HOME", sandbox.path()); environment.insert("KDE_SKIP_KDERC", "1"); environment.insert("CONTROL_POWER_CASCADE_CHILD", "1");
        QProcess child; child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("fullCascadeChild")});
        QVERIFY(child.waitForStarted()); QVERIFY(child.waitForFinished(30000));
        const auto output = child.readAllStandardOutput() + child.readAllStandardError();
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, output.constData());
    }
    void fullCascadeChild()
    {
        if (!qEnvironmentVariableIsSet("CONTROL_POWER_CASCADE_CHILD")) QSKIP("Requires isolated pre-application XDG roots.");
        backend.reset();
        file = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/powerdevilrc");
        const auto globalFile = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/kdeglobals");
        const auto defaultsFile = qEnvironmentVariable("XDG_CONFIG_DIRS") + QStringLiteral("/powerdevilrc");
        auto save = [](const QString &path, const QByteArray &data) {
            QFile output(path);
            if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.flush()) return false;
            // KF global parse caches use millisecond mtimes. Give setup data a
            // distinct old timestamp so a native save cannot share its tag.
            static qint64 setupTimestamp=1000000;
            return output.setFileTime(QDateTime::fromMSecsSinceEpoch(++setupTimestamp), QFileDevice::FileModificationTime);
        };
        QVERIFY(save(file, "[AC][Display]\nTurnOffDisplayIdleTimeoutSec=1200\n"));
        QVERIFY(save(defaultsFile, "[AC][Display]\nTurnOffDisplayIdleTimeoutSec=1000\n"));
        QVERIFY(save(globalFile, "[AC][SuspendAndShutdown]\nPowerButtonAction=8\n[Other]\nKeep=global-unowned\n"));
        daemon = new FakePowerDaemon;
        KdePowerPolicyBackend::ConfigContext context; context.fullCascade = true;
        backend = std::make_unique<KdePowerPolicyBackend>(file, daemon, context);
        auto before = read(); auto request = before; QString error;
        auto key = PowerPolicyKey::DisplaySeconds;
        QVERIFY(before.profiles[0].keys.value(key).inheritedPresent);
        QCOMPARE(before.profiles[0].keys.value(key).defaultValue.toInt(), 1000);
        QVERIFY(PowerPolicyFields::restore(request.profiles[0], before.profiles[0], key, error));
        backend->writePolicy(before, request); QCOMPARE(daemon->reloads, 1); daemon->acknowledge();
        { KConfig local(file, KConfig::SimpleConfig); QVERIFY(local.group("AC").group("Display").hasKey("TurnOffDisplayIdleTimeoutSec")); QCOMPARE(local.group("AC").group("Display").readEntry("TurnOffDisplayIdleTimeoutSec", 0), 1000); }
        // A global-origin native reset writes the user global file, preserving
        // unrelated entries. This cannot be simulated using NoGlobals sources.
        before = read(); request = before; key = PowerPolicyKey::PowerButtonAction;
        const auto state = before.profiles[0].keys.value(key);
        QVERIFY(state.globalPresent && !state.localPresent && !state.inheritedPresent);
        QVERIFY(PowerPolicyFields::restore(request.profiles[0], before.profiles[0], key, error));
        backend->writePolicy(before, request); QCOMPARE(daemon->reloads, 2); daemon->acknowledge();
        { KConfig global(globalFile, KConfig::SimpleConfig); QVERIFY(!global.group("AC").group("SuspendAndShutdown").hasKey("PowerButtonAction")); QCOMPARE(global.group("Other").readEntry("Keep", QString()), QStringLiteral("global-unowned")); }
        // Local shadowing a global override uses native deletion-mask semantics.
        QVERIFY(save(globalFile, "[AC][SuspendAndShutdown]\nPowerButtonAction=32\n[Other]\nKeep=global-unowned\n"));
        { KConfig local(file, KConfig::SimpleConfig); local.group("AC").group("SuspendAndShutdown").writeEntry("PowerButtonAction", 8u); QVERIFY(local.sync()); }
        before = read(); request = before;
        QVERIFY(before.profiles[0].keys.value(key).globalPresent && before.profiles[0].keys.value(key).localPresent);
        QVERIFY(PowerPolicyFields::restore(request.profiles[0], before.profiles[0], key, error));
        backend->writePolicy(before, request); QCOMPARE(daemon->reloads, 3); daemon->acknowledge();
        const auto after = read(); QVERIFY(!after.profiles[0].keys.value(key).effectivePresent);
        QVERIFY(!after.profiles[0].keys.value(key).localPresent); QVERIFY(after.profiles[0].keys.value(key).globalPresent);
        { KConfig global(globalFile, KConfig::SimpleConfig); QCOMPARE(global.group("AC").group("SuspendAndShutdown").readEntry("PowerButtonAction", 0u), 32u); }
        { QFile local(file); QVERIFY(local.open(QIODevice::ReadOnly)); QVERIFY(local.readAll().contains("PowerButtonAction[$d]")); }
        // Per-key global provenance changes invalidate a pending local Set even
        // when a local deletion mask hides the changed global value.
        before = read(); request = before;
        QVERIFY(PowerPolicyFields::stage(request.profiles[0], before.profiles[0], key, QVariant(8u), error));
        { KConfig global(globalFile, KConfig::SimpleConfig); global.group("AC").group("SuspendAndShutdown").writeEntry("PowerButtonAction", 64u); QVERIFY(global.sync()); }
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::writeFinished);
        backend->writePolicy(before, request); QCOMPARE(finished.size(), 1); QVERIFY(!finished.first()[0].toBool()); QCOMPARE(daemon->reloads, 3);
        // Native inherited immutable policy applies independently to each key.
        QVERIFY(save(defaultsFile, "[AC][Display]\nTurnOffDisplayIdleTimeoutSec[$i]=1000\n"));
        const auto locked = read(); QVERIFY(locked.profiles[0].keys.value(PowerPolicyKey::DisplaySeconds).immutable);
        QVERIFY(!locked.profiles[0].keys.value(PowerPolicyKey::DisplaySeconds).writable);
        QVERIFY(locked.profiles[0].keys.value(PowerPolicyKey::DisplayEnabled).writable);
    }

private:
    static void explicitChanges(const PowerPolicySnapshot &baseline, PowerPolicySnapshot &request)
    {
        for (int i = 0; i < request.profiles.size(); ++i)
            for (const auto key : PowerPolicyFields::profileKeys())
                if (PowerPolicyFields::value(request.profiles[i], key) != PowerPolicyFields::value(baseline.profiles[i], key))
                    request.profiles[i].intents.insert(key, PowerPolicyIntent::Set);
        for (const auto key : PowerPolicyFields::globalKeys())
            if (PowerPolicyFields::value(request, key) != PowerPolicyFields::value(baseline, key))
                request.intents.insert(key, PowerPolicyIntent::Set);
    }
    static bool has(const QList<PowerPolicyAction> &choices, uint value) {
        return std::any_of(choices.cbegin(), choices.cend(), [value](const auto &item) { return item.value == value; });
    }
    PowerPolicySnapshot read()
    {
        QSignalSpy finished(backend.get(), &PowerPolicyBackend::readFinished);
        backend->readCurrent();
        if (finished.size() != 1 || !finished.first()[0].toBool())
            qFatal("Temporary PowerDevil configuration read failed");
        return qvariant_cast<PowerPolicySnapshot>(finished.first()[1]);
    }
    std::unique_ptr<QTemporaryDir> dir;
    QString file;
    FakePowerDaemon *daemon = nullptr;
    std::unique_ptr<KdePowerPolicyBackend> backend;
};
QTEST_GUILESS_MAIN(PowerPolicyBackendTest)
#include "power_policy_backend_test.moc"
