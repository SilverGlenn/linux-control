#include <QtTest>
#include "settings/InputSettings.h"

static InputSnapshot fixture()
{
    InputDevice d{"event4", "Test Touchpad", "/org/kde/KWin/InputDevice/event4", {
        {"name", "Test Touchpad"}, {"sysName", "event4"}, {"vendor", 12u}, {"product", 34u},
        {"pointer", true}, {"touchpad", true}, {"isVirtual", false},
        {"enabled", true}, {"supportsDisableEvents", true}, {"enabledByDefault", true},
        {"leftHanded", false}, {"supportsLeftHanded", true}, {"leftHandedEnabledByDefault", false},
        {"pointerAcceleration", 0.0}, {"supportsPointerAcceleration", true}, {"defaultPointerAcceleration", 0.25},
        {"pointerAccelerationProfileFlat", false}, {"pointerAccelerationProfileAdaptive", true},
        {"supportsPointerAccelerationProfileFlat", true}, {"supportsPointerAccelerationProfileAdaptive", true},
        {"defaultPointerAccelerationProfileFlat", false}, {"defaultPointerAccelerationProfileAdaptive", true},
        {"scrollFactor", 1.5}, {"naturalScroll", false}, {"supportsNaturalScroll", true}, {"naturalScrollEnabledByDefault", true},
        {"scrollTwoFinger", true}, {"scrollEdge", false}, {"scrollOnButtonDown", false},
        {"supportsScrollTwoFinger", true}, {"supportsScrollEdge", true}, {"supportsScrollOnButtonDown", false},
        {"scrollTwoFingerEnabledByDefault", true}, {"scrollEdgeEnabledByDefault", false},
        {"__writable", QStringList{"enabled", "leftHanded", "pointerAcceleration", "pointerAccelerationProfileFlat", "pointerAccelerationProfileAdaptive", "scrollFactor", "naturalScroll", "scrollTwoFinger", "scrollEdge"}}
    }};
    InputSnapshot s; s.available = s.writable = true; s.owner = ":1.8"; s.version = "6.6.6"; s.devices = {d}; return s;
}
class FakeInputBackend final : public InputBackend {
public:
    int reads = 0, writes = 0;
    bool delay = false;
    quint64 pendingToken = 0;
    InputSnapshot value = fixture(), requested;
    void readCurrent(quint64 token) override { ++reads; emit readFinished(token, true, value, {}); }
    void writeChanges(quint64 token, const InputSnapshot &, const InputSnapshot &request) override {
        ++writes; pendingToken = token; requested = request;
        if (!delay) { value = request; emit writeFinished(token, InputOutcome::Success, value, {}); }
    }
    void changed() { emit externalChange(); }
    void finish(InputOutcome result) { emit writeFinished(pendingToken, result, value, "Some values may have changed"); }
};
class InputSettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void stagedApplyAndDefaults()
    {
        auto *backend = new FakeInputBackend; InputSettings session(backend);
        QTRY_VERIFY(session.isReady());
        session.selectProperty("event4", "leftHanded", true);
        QCOMPARE(backend->writes, 0); QVERIFY(session.isDirty());
        session.selectProperty("event4", "pointerAccelerationProfileFlat", true);
        QVERIFY(!session.staged().device("event4")->properties["pointerAccelerationProfileAdaptive"].toBool());
        session.selectProperty("event4", "scrollEdge", true);
        QVERIFY(!session.staged().device("event4")->properties["scrollTwoFinger"].toBool());
        QSignalSpy done(&session, &SettingsSession::applyFinished); session.apply();
        QCOMPARE(done.size(), 1); QVERIFY(done[0][0].toBool()); QVERIFY(!session.isDirty());
        session.restoreDeviceDefaults("event4");
        QVERIFY(!session.staged().device("event4")->properties["leftHanded"].toBool());
        QCOMPARE(session.staged().device("event4")->properties["scrollFactor"].toDouble(), 1.5);
        QCOMPARE(session.staged().device("event4")->properties["pointerAcceleration"].toDouble(), 0.25);
        session.revert(); QVERIFY(!session.isDirty());
    }
    void rejectsWrongTypesUnknownAndUnsupportedProperties()
    {
        auto *backend = new FakeInputBackend; InputSettings session(backend); QTRY_VERIFY(session.isReady());
        session.selectProperty("event4", "leftHanded", "true"); QVERIFY(!session.isDirty());
        session.selectProperty("event4", "pointerAcceleration", 3.0); QVERIFY(!session.isDirty());
        session.selectProperty("event4", "pointerAcceleration", 0.12345); QVERIFY(!session.isDirty());
        session.selectProperty("event4", "tabletToolIsRelative", true); QVERIFY(!session.isDirty());
        session.selectProperty("event4", "scrollOnButtonDown", true); QVERIFY(!session.isDirty());
        QCOMPARE(backend->writes, 0);
    }
    void externalConflictPreservesChoices()
    {
        auto *backend = new FakeInputBackend; InputSettings session(backend); QTRY_VERIFY(session.isReady());
        session.selectProperty("event4", "leftHanded", true);
        backend->value.devices[0].properties["pointerAcceleration"] = 0.5; backend->changed();
        QVERIFY(session.hasConflict()); QVERIFY(session.staged().device("event4")->properties["leftHanded"].toBool());
        session.selectProperty("event4", "leftHanded", false);
        QVERIFY(session.staged().device("event4")->properties["leftHanded"].toBool());
        QVERIFY(!session.errorText().isEmpty());
        session.apply(); QCOMPARE(backend->writes, 0);
        session.revert(); QVERIFY(!session.hasConflict()); QVERIFY(!session.isDirty());
        QCOMPARE(session.applied().devices[0].properties["pointerAcceleration"].toDouble(), 0.5);
    }
    void partialAndUncertainResultsRemainTruthful()
    {
        auto *backend = new FakeInputBackend; backend->delay = true;
        InputSettings session(backend); QTRY_VERIFY(session.isReady());
        session.selectProperty("event4", "leftHanded", true); session.apply(); QVERIFY(session.isBusy());
        session.revert(); QVERIFY(session.isBusy());
        backend->finish(InputOutcome::Uncertain); QVERIFY(session.hasConflict()); QVERIFY(!session.canApply());
        session.revert(); QVERIFY(!session.canApply());
        backend->value.owner = ":1.99"; backend->changed(); QVERIFY(session.canApply());
    }
    void wrongSuccessCannotClearPendingChoices()
    {
        auto *backend = new FakeInputBackend; backend->delay = true;
        InputSettings session(backend); QTRY_VERIFY(session.isReady());
        session.selectProperty("event4", "leftHanded", true); session.apply();
        backend->finish(InputOutcome::Success);
        QCOMPARE(session.lastOutcome(), InputOutcome::Uncertain);
        QVERIFY(session.isDirty()); QVERIFY(session.hasConflict()); QVERIFY(!session.canApply());
        QVERIFY(session.resultText().isEmpty());
    }
    void replacementOwnerReleasesUncertaintyWithoutExtraRestart()
    {
        auto *backend = new FakeInputBackend; backend->delay = true;
        InputSettings session(backend); QTRY_VERIFY(session.isReady());
        session.selectProperty("event4", "leftHanded", true); session.apply();
        backend->value.owner = ":1.200"; backend->finish(InputOutcome::Uncertain);
        session.revert(); QVERIFY(session.canApply());
    }
    void hotUnplugKeepsPendingChoicesWithoutWriting()
    {
        auto *backend = new FakeInputBackend; InputSettings session(backend); QTRY_VERIFY(session.isReady());
        session.selectProperty("event4", "leftHanded", true); backend->value.devices.clear(); backend->changed();
        QVERIFY(session.hasConflict()); QCOMPARE(session.staged().devices.size(), 1);
        session.apply(); QCOMPARE(backend->writes, 0); session.revert(); QVERIFY(session.staged().devices.isEmpty());
    }
};
QTEST_GUILESS_MAIN(InputSettingsTest)
#include "input_settings_test.moc"
