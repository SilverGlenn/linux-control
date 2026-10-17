#include <QtTest>
#include "audio_fixture.h"
#include "operations/OperationManager.h"

class AudioSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void scalingPreservesBalanceAndUsesPulseUnits() {
        auto object = audioFixture().sinks.first();
        QCOMPARE(AudioTypes::scaledVolumes(object, 50), QVector<quint32>({32768, 16384}));
        QCOMPARE(AudioTypes::scaledVolumes(object, 150), QVector<quint32>({98304, 49152}));
        QVERIFY(AudioTypes::scaledVolumes(object, 151).isEmpty());
        object.channelVolumes = {0, 0};
        QCOMPARE(AudioTypes::scaledVolumes(object, 100), QVector<quint32>({65536, 65536}));
        object.channelVolumes = {65536, 32768}; QCOMPARE(object.volumePercent(), 100);
    }
    void validationRejectsMissingUnavailableAndWrongKinds() {
        auto snapshot = audioFixture(); QString error;
        auto request = audioVolumeRequest(snapshot, 50);
        QVERIFY(AudioTypes::validateRequest(snapshot, request, error));
        request.identity = QStringLiteral("reused-index"); QVERIFY(!AudioTypes::validateRequest(snapshot, request, error));
        request = audioVolumeRequest(snapshot, 50); request.action = AudioAction::SetPort; request.choice = QStringLiteral("headphones");
        QVERIFY(!AudioTypes::validateRequest(snapshot, request, error));
        request.choice = QStringLiteral("analog-output"); QVERIFY(AudioTypes::validateRequest(snapshot, request, error));
        request.action = AudioAction::SetProfile; QVERIFY(!AudioTypes::validateRequest(snapshot, request, error));
        request.kind = AudioObjectKind::Card; request.index = 2; request.identity = snapshot.cards[0].identity; request.choice = QStringLiteral("off");
        QVERIFY(AudioTypes::validateRequest(snapshot, request, error));
        request.choice = QStringLiteral("--server"); QVERIFY(!AudioTypes::validateRequest(snapshot, request, error));
        snapshot.writable = false; snapshot.unavailableReason = QStringLiteral("Write lock");
        QVERIFY(!AudioTypes::validateRequest(snapshot, request, error)); QCOMPARE(error, QStringLiteral("Write lock"));
    }
    void preflightIsTargetedAndGuardsServerAndChannelIdentity() {
        auto expected = audioFixture(), current = expected;
        auto request = audioVolumeRequest(expected, 50);
        current.sinks[1].mute = true; QVERIFY(AudioTypes::sameTarget(expected, current, request));
        current.sinks[0].channelVolumes[1]++; QVERIFY(!AudioTypes::sameTarget(expected, current, request));
        current = expected; current.sinks[0].identity = QStringLiteral("reused"); QVERIFY(!AudioTypes::sameTarget(expected, current, request));
        current = expected; current.serverCookie = QStringLiteral("aaaa:bbbb"); QVERIFY(!AudioTypes::sameTarget(expected, current, request));
        current = expected; current.sinks[0].channelNames.swapItemsAt(0, 1); QVERIFY(!AudioTypes::sameTarget(expected, current, request));
    }
    void volumeReadbackChecksEveryChannelAndRoutingChecksDestination() {
        auto snapshot = audioFixture(); auto request = audioVolumeRequest(snapshot, 50);
        request.action = AudioAction::SetChannelVolumes; request.channelVolumes = {32768, 16384};
        snapshot.sinks[0].channelVolumes = request.channelVolumes; QVERIFY(AudioTypes::matchesRequest(snapshot, request));
        snapshot.sinks[0].channelVolumes[1]++; QVERIFY(AudioTypes::matchesRequest(snapshot, request));
        snapshot.sinks[0].channelVolumes[1] += 3; QVERIFY(!AudioTypes::matchesRequest(snapshot, request));
        request.action = AudioAction::MoveStream; request.kind = AudioObjectKind::SinkInput; request.index = 10;
        request.identity = snapshot.sinkInputs[0].identity; request.destinationIndex = 5; request.destinationIdentity = snapshot.sinks[1].identity;
        snapshot.sinkInputs[0].routeIndex = 5; QVERIFY(AudioTypes::matchesRequest(snapshot, request));
        snapshot.sinks[1].identity = QStringLiteral("reused-destination"); QVERIFY(!AudioTypes::matchesRequest(snapshot, request));
    }
    void confirmedStateAndOutcomeOutliveRequests() {
        auto *backend = new IntegrationAudioBackend; AudioSettings settings(backend);
        QTRY_VERIFY(settings.isReady()); QVERIFY(!settings.isBusy());
        settings.requestChange(audioVolumeRequest(settings.snapshot(), 50));
        QVERIFY(settings.isBusy()); QCOMPARE(settings.snapshot().sinks[0].volumePercent(), 100);
        QVERIFY(OperationManager::instance()->hasActiveJobs());
        backend->complete(); QVERIFY(!settings.isBusy());
        QCOMPARE(settings.snapshot().sinks[0].channelVolumes, QVector<quint32>({32768, 16384}));
        QVERIFY(!settings.resultText().isEmpty()); QVERIFY(settings.errorText().isEmpty());
        settings.refresh(); QVERIFY(!settings.resultText().isEmpty());
    }
    void failedReadDisablesStaleControlsThenRecovers() {
        auto *backend = new IntegrationAudioBackend; AudioSettings settings(backend); QTRY_VERIFY(settings.isReady());
        backend->failRead = true; settings.refresh(); QVERIFY(!settings.isReady()); QVERIFY(!settings.snapshot().writable);
        settings.requestChange(audioVolumeRequest(backend->current, 20)); QCOMPARE(backend->writes, 0);
        backend->failRead = false; settings.refresh(); QVERIFY(settings.isReady()); QVERIFY(settings.snapshot().writable);
        QVERIFY(settings.errorText().isEmpty());
    }
    void uncertainResultCannotUnlockThroughRefresh() {
        auto *backend = new IntegrationAudioBackend; AudioSettings settings(backend); QTRY_VERIFY(settings.isReady());
        settings.requestChange(audioVolumeRequest(settings.snapshot(), 50)); backend->complete(AudioOutcome::Uncertain, false);
        QVERIFY(!settings.snapshot().writable); QVERIFY(settings.supportText().contains(QStringLiteral("restarts")));
        settings.refresh(); QVERIFY(settings.isReady()); QVERIFY(!settings.snapshot().writable);
        settings.requestChange(audioVolumeRequest(settings.snapshot(), 25)); QCOMPARE(backend->writes, 1);
        backend->current.serverCookie = QStringLiteral("aaaa:bbbb"); settings.refresh(); QVERIFY(settings.snapshot().writable);
    }
    void incorrectSuccessIsNotReportedAndLocksWrites() {
        auto *backend = new IntegrationAudioBackend; AudioSettings settings(backend); QTRY_VERIFY(settings.isReady());
        settings.requestChange(audioVolumeRequest(settings.snapshot(), 50)); backend->complete(AudioOutcome::Success, false);
        QVERIFY(settings.resultText().isEmpty()); QVERIFY(!settings.errorText().isEmpty()); QVERIFY(!settings.snapshot().writable);
        settings.refresh(); QVERIFY(!settings.snapshot().writable);
    }
    void obsoleteCompletionsCannotFinishNewJob() {
        auto *backend = new IntegrationAudioBackend; AudioSettings settings(backend); QTRY_VERIFY(settings.isReady());
        settings.requestChange(audioVolumeRequest(settings.snapshot(), 50));
        emit backend->writeFinished(backend->pendingToken - 1, AudioOutcome::Success, backend->current, {});
        QVERIFY(settings.isBusy()); backend->complete(); QVERIFY(!settings.isBusy());
    }
};
QTEST_MAIN(AudioSettingsTest)
#include "audio_settings_test.moc"
