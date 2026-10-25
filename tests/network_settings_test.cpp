#include <QtTest>
#include <QSignalSpy>
#include "settings/NetworkSettings.h"

namespace {
NetworkSnapshot fixture()
{
    NetworkSnapshot snapshot;
    snapshot.version = QStringLiteral("1.54.3"); snapshot.serviceOwner = QStringLiteral(":1.42");
    snapshot.serviceAvailable = true; snapshot.checkpointAvailable = true;
    NetworkDevice device;
    device.path = QStringLiteral("/devices/1"); device.interfaceName = QStringLiteral("eth0");
    device.type = 1; device.managed = true; device.carrier = true; device.state = 100;
    device.activeConnectionPath = QStringLiteral("/active/1"); device.activeProfilePath = QStringLiteral("/profiles/1");
    device.availableProfilePaths = {QStringLiteral("/profiles/1"), QStringLiteral("/profiles/2")};
    snapshot.devices = {device};
    for (int index = 1; index <= 2; ++index) {
        NetworkProfile profile;
        profile.path = QStringLiteral("/profiles/%1").arg(index); profile.id = QStringLiteral("Wired %1").arg(index);
        profile.uuid = QStringLiteral("uuid-%1").arg(index); profile.type = QStringLiteral("802-3-ethernet");
        profile.editable = true; profile.versionId = 5;
        profile.settings = {
            {QStringLiteral("connection"), {{QStringLiteral("id"), profile.id}, {QStringLiteral("uuid"), profile.uuid}, {QStringLiteral("type"), profile.type}, {QStringLiteral("autoconnect"), true}}},
            {QStringLiteral("ipv4"), {{QStringLiteral("method"), QStringLiteral("auto")}}},
            {QStringLiteral("ipv6"), {{QStringLiteral("method"), QStringLiteral("auto")}}}
        };
        snapshot.profiles.append(profile);
    }
    return snapshot;
}
NetworkSnapshot updated(const NetworkSnapshot &original, const NetworkRequest &request)
{
    auto snapshot = original;
    for (auto &profile : snapshot.profiles) if (profile.path == request.profilePath) {
        profile.settings = NetworkTypes::mergePreferences(profile, request.preferences);
        profile.id = request.preferences.id;
        ++profile.versionId;
    }
    return snapshot;
}
}

class FakeNetworkBackend : public NetworkBackend {
public:
    int reads = 0, changes = 0, keeps = 0, reverts = 0, scans = 0;
    NetworkSnapshot baseline;
    NetworkRequest request;
    QString restriction;
    QString connectivityRestrictionReason() const override { return restriction; }
    void readCurrent() override { ++reads; }
    void scan(const QString &) override { ++scans; }
    void beginChange(const NetworkSnapshot &snapshot, const NetworkRequest &change) override
    { ++changes; baseline = snapshot; request = change; }
    void keepChange() override { ++keeps; }
    void revertChange() override { ++reverts; }
    void read(const NetworkSnapshot &snapshot) { emit readFinished(true, snapshot, {}); }
    void preview(const NetworkSnapshot &snapshot, int seconds = 30) { emit previewReady(snapshot, seconds); }
    void finish(bool kept, bool restored, const NetworkSnapshot &snapshot, const QString &error = {})
    { emit transactionFinished(kept, restored, snapshot, error); }
};

class NetworkSettingsTest : public QObject {
    Q_OBJECT
    FakeNetworkBackend *backend = nullptr;
    NetworkSettings *session = nullptr;
    void stageRename()
    {
        auto prefs = session->stagedPreferences(); prefs.id = QStringLiteral("Renamed");
        session->stagePreferences(prefs);
    }
    void beginEdit()
    {
        stageRename(); session->apply(); backend->read(fixture());
        QCOMPARE(backend->changes, 1);
    }
private slots:
    void candidateRestrictionBlocksSessionRoutesButKeepsInactiveStaging()
    {
        backend->restriction=QStringLiteral("Connection changes withheld in this candidate.");
        QVERIFY(session->rollbackAvailable()); QVERIFY(!session->selectedProfileEditReason().isEmpty());
        stageRename(); QVERIFY(!session->isDirty());
        session->activateProfile(QStringLiteral("/profiles/2"),QStringLiteral("/devices/1"));
        session->disconnectDevice(QStringLiteral("/devices/1"));
        session->connectWifi(QStringLiteral("/aps/1"),QStringLiteral("private"));
        QCOMPARE(backend->changes,0); QVERIFY(session->errorText().contains("withheld"));
        session->selectProfile(QStringLiteral("/profiles/2"));
        QVERIFY(session->selectedProfileEditReason().isEmpty());
        stageRename(); QVERIFY(session->isDirty()); session->apply(); backend->read(fixture());
        QCOMPARE(backend->changes,1); QVERIFY(backend->request.devicePath.isEmpty());
    }
    void init()
    {
        backend = new FakeNetworkBackend; session = new NetworkSettings(backend);
        QTRY_COMPARE(backend->reads, 1);
        backend->read(fixture());
    }
    void cleanup() { delete session; }
    void stagingAndProfileGuardDoNotWrite()
    {
        stageRename(); QVERIFY(session->isDirty()); QCOMPARE(backend->changes, 0);
        session->selectProfile(QStringLiteral("/profiles/2"));
        QCOMPARE(session->selectedProfile(), QStringLiteral("/profiles/1"));
        QVERIFY(session->errorText().contains(QStringLiteral("revert")));
        session->disconnectDevice(QStringLiteral("/devices/1"));
        QCOMPARE(backend->changes, 0);
        session->revert(); backend->read(fixture());
        QVERIFY(!session->isDirty());
        session->selectProfile(QStringLiteral("/profiles/2"));
        QCOMPARE(session->selectedProfile(), QStringLiteral("/profiles/2"));
    }
    void keepCompletesOnlyAfterAuthoritativeReadback()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        beginEdit(); const auto final = updated(fixture(), backend->request);
        backend->preview(final);
        QVERIFY(session->awaitingConfirmation()); QVERIFY(session->isBusy()); QVERIFY(finished.isEmpty());
        session->keep(); session->keep(); QCOMPARE(backend->keeps, 1);
        QVERIFY(finished.isEmpty());
        backend->finish(true, false, final);
        QVERIFY(!session->isBusy()); QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
        QCOMPARE(finished.size(), 1); QVERIFY(finished.first().first().toBool());
        QCOMPARE(session->snapshot().profile(QStringLiteral("/profiles/1"))->id, QStringLiteral("Renamed"));
    }
    void inactiveProfileCanSaveWithoutPreviewOrCheckpoint()
    {
        auto snapshot = fixture(); snapshot.checkpointAvailable = false;
        session->refresh(); backend->read(snapshot);
        session->selectProfile(QStringLiteral("/profiles/2")); stageRename();
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->apply(); backend->read(snapshot);
        QCOMPARE(backend->changes, 1); QVERIFY(!session->awaitingConfirmation());
        backend->finish(true, false, updated(snapshot, backend->request));
        QVERIFY(!session->isBusy()); QVERIFY(!session->hasConflict());
        QVERIFY(finished.first().first().toBool());
        QCOMPARE(backend->keeps, 0);
        QVERIFY(session->resultText().contains(QStringLiteral("current connection was not changed")));
    }
    void definiteFailurePreservesDraftAndAllowsRetry()
    {
        session->selectProfile(QStringLiteral("/profiles/2"));
        beginEdit();
        backend->finish(false, true, fixture(), QStringLiteral("Authorization was canceled."));
        QVERIFY(session->isDirty()); QVERIFY(!session->hasConflict());
        QCOMPARE(session->stagedPreferences().id, QStringLiteral("Renamed"));
        QVERIFY(session->errorText().contains(QStringLiteral("canceled")));
        session->apply(); backend->read(fixture());
        QCOMPARE(backend->changes, 2);
    }
    void uncertainFailurePreservesDraftUntilExplicitRevert()
    {
        beginEdit();
        backend->finish(false, false, fixture(), QStringLiteral("The outcome is uncertain."));
        QVERIFY(session->isDirty()); QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedPreferences().id, QStringLiteral("Renamed"));
        session->revert(); backend->read(fixture());
        QVERIFY(!session->isDirty()); QVERIFY(!session->hasConflict());
    }
    void unavailableCleanApplyDoesNotReportSuccess()
    {
        session->refresh(); emit backend->readFinished(false, {}, QStringLiteral("No service"));
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->apply();
        QCOMPARE(finished.size(), 1); QVERIFY(!finished.first().first().toBool());
    }
    void unresolvedBackendReasonTakesPriorityOverGenericCapabilities()
    {
        auto snapshot = fixture(); snapshot.checkpointAvailable = false;
        snapshot.reason = QStringLiteral("The operation is unresolved. Restart NetworkManager before changing settings.");
        for (auto &profile : snapshot.profiles) profile.editable = false;
        session->refresh(); backend->read(snapshot);
        QCOMPARE(session->supportText(), snapshot.reason);
        QVERIFY(!session->supportText().contains(QStringLiteral("can still be edited")));
    }
    void timeoutAndRepeatedRevertWaitForRollbackReadback()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        beginEdit(); backend->preview(updated(fixture(), backend->request), 1);
        QTRY_COMPARE_WITH_TIMEOUT(backend->reverts, 1, 2500);
        session->revert(); QCOMPARE(backend->reverts, 1);
        QVERIFY(session->isBusy()); QVERIFY(finished.isEmpty());
        auto restored = fixture(); restored.profiles[0].versionId = 9;
        restored.devices[0].activeConnectionPath = QStringLiteral("/active/3");
        backend->finish(false, true, restored);
        QVERIFY(!session->isBusy()); QVERIFY(!session->hasConflict());
        QVERIFY(session->resultText().contains(QStringLiteral("No changes were kept")));
        QVERIFY(!finished.first().first().toBool());
    }
    void preflightRejectsExternalEditsAndPreservesDraft()
    {
        stageRename(); session->apply();
        auto external = fixture();
        external.profiles[0].settings[QStringLiteral("ipv4")].insert(QStringLiteral("route-metric"), qint64(77));
        external.profiles[0].versionId++;
        backend->read(external);
        QCOMPARE(backend->changes, 0); QVERIFY(session->hasConflict()); QVERIFY(session->isDirty());
        QCOMPARE(session->stagedPreferences().id, QStringLiteral("Renamed"));
        session->apply(); QCOMPARE(backend->changes, 0);
        session->revert(); backend->read(external);
        QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty());
    }
    void invalidReadAndFalseSuccessAreVisible()
    {
        stageRename(); session->apply();
        emit backend->readFinished(false, {}, QStringLiteral("Service disappeared"));
        QCOMPARE(backend->changes, 0); QVERIFY(!session->isReady());
        QVERIFY(session->isDirty()); QVERIFY(session->errorText().contains(QStringLiteral("disappeared")));
        session->revert(); backend->read(fixture());
        beginEdit(); backend->finish(true, false, fixture());
        QVERIFY(session->hasConflict()); QVERIFY(!session->errorText().isEmpty());
        QVERIFY(session->resultText().isEmpty());
    }
    void invalidPreviewRequestsRollbackOnce()
    {
        beginEdit(); backend->preview(fixture());
        QCOMPARE(backend->reverts, 1); QVERIFY(!session->awaitingConfirmation());
        backend->preview(fixture()); QCOMPARE(backend->reverts, 1);
        backend->finish(false, true, fixture(), QStringLiteral("Verification failed"));
        QVERIFY(!session->hasConflict());
        QCOMPARE(session->errorText(), QStringLiteral("Verification failed"));
    }
    void sessionOutlivesAnyObservingView()
    {
        auto *view = new QObject;
        connect(session, &SettingsSession::stateChanged, view, [] {});
        beginEdit(); delete view;
        const auto final = updated(fixture(), backend->request);
        backend->preview(final); QVERIFY(session->awaitingConfirmation());
        session->keep(); backend->finish(true, false, final);
        QVERIFY(!session->isBusy()); QCOMPARE(backend->keeps, 1);
    }
    void scanIsReadOnlyAndErrorsRemainVisibleAfterRefresh()
    {
        auto snapshot = fixture(); snapshot.devices[0].type = 2;
        session->refresh(); backend->read(snapshot);
        session->scan(QStringLiteral("/devices/1"));
        QCOMPARE(backend->scans, 1); QCOMPARE(backend->changes, 0); QVERIFY(session->isScanning());
        emit backend->scanFinished(false, QStringLiteral("Wi-Fi is disabled"));
        QVERIFY(!session->isScanning());
        session->refresh(); backend->read(snapshot);
        QCOMPARE(session->errorText(), QStringLiteral("Wi-Fi is disabled"));
    }
    void unsupportedSecurityNeverDispatches()
    {
        auto snapshot = fixture(); snapshot.devices[0].type = 2;
        NetworkAccessPoint ap; ap.path = QStringLiteral("/ap/1"); ap.devicePath = snapshot.devices[0].path;
        ap.ssid = QByteArrayLiteral("enterprise"); ap.security = NetworkAccessPoint::Security::Unsupported;
        snapshot.accessPoints = {ap};
        session->refresh(); backend->read(snapshot);
        session->connectWifi(ap.path, QStringLiteral("secret123"));
        QCOMPARE(backend->changes, 0); QVERIFY(!session->errorText().isEmpty());
    }
};
QTEST_GUILESS_MAIN(NetworkSettingsTest)
#include "network_settings_test.moc"
