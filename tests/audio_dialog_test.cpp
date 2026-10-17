#include <QtTest>
#include <AeroQt/stylesheet.h>
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QPalette>
#include <algorithm>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>

#include "ui/dialogs/SoundDialog.h"
#include "settings/AudioSettings.h"

namespace {
AudioSnapshot fixture()
{
    AudioSnapshot snapshot;
    snapshot.available = snapshot.writable = true;
    snapshot.serverCookie = QStringLiteral("fixture-server");
    snapshot.serverName = QStringLiteral("PulseAudio (on PipeWire)");
    snapshot.serverVersion = QStringLiteral("1.4.fixture");
    for (int i = 0; i < 2; ++i) {
        AudioObject sink;
        sink.kind = AudioObjectKind::Sink;
        sink.index = quint32(i + 1);
        sink.name = QStringLiteral("output-%1").arg(i);
        sink.description = i == 0 ? QStringLiteral("Speakers") : QStringLiteral("Headphones");
        sink.identity = QStringLiteral("sink-identity-%1").arg(i);
        sink.channelVolumes = {65536, 32768};
        sink.channelNames = {QStringLiteral("front-left"), QStringLiteral("front-right")};
        sink.hasCard = true; sink.cardIndex = 10;
        sink.activePort = QStringLiteral("speaker");
        sink.ports = {{QStringLiteral("speaker"), QStringLiteral("Speakers"), QStringLiteral("yes")},
                      {QStringLiteral("headphones"), QStringLiteral("Headphones"), QStringLiteral("unknown")},
                      {QStringLiteral("unplugged"), QStringLiteral("Disconnected port"), QStringLiteral("no")}};
        snapshot.sinks << sink;
        AudioObject source = sink;
        source.kind = AudioObjectKind::Source; source.index = quint32(3 + i);
        source.name = QStringLiteral("input-%1").arg(i);
        source.identity = QStringLiteral("source-identity-%1").arg(i);
        source.description = i == 0 ? QStringLiteral("Microphone") : QStringLiteral("Headset microphone");
        snapshot.sources << source;
    }
    snapshot.defaultSink = snapshot.sinks[0].name;
    snapshot.defaultSource = snapshot.sources[0].name;
    AudioObject card;
    card.kind = AudioObjectKind::Card; card.index = 10;
    card.name = QStringLiteral("sound-card"); card.identity = QStringLiteral("card-identity");
    card.activeProfile = QStringLiteral("stereo");
    card.profiles = {{QStringLiteral("stereo"), QStringLiteral("Analog Stereo Duplex"), QStringLiteral("yes")},
                     {QStringLiteral("surround"), QStringLiteral("Digital Surround"), QStringLiteral("unknown")}};
    snapshot.cards << card;
    AudioObject application;
    application.kind = AudioObjectKind::SinkInput; application.index = 20;
    application.name = QStringLiteral("browser-stream"); application.description = QStringLiteral("Browser — media");
    application.identity = QStringLiteral("browser-identity");
    application.channelVolumes = {32768, 32768};
    application.channelNames = {QStringLiteral("front-left"), QStringLiteral("front-right")};
    application.hasRoute = true; application.routeIndex = 1;
    snapshot.sinkInputs << application;
    application.kind = AudioObjectKind::SourceOutput; application.index = 21;
    application.name = QStringLiteral("call-stream"); application.description = QStringLiteral("Call — microphone");
    application.identity = QStringLiteral("call-identity"); application.routeIndex = 3;
    snapshot.sourceOutputs << application;
    return snapshot;
}
void capture(QWidget &widget, const QString &name)
{
    const QString directory = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    QVERIFY(QDir().mkpath(directory));
    widget.show(); QApplication::processEvents();
    QVERIFY(widget.grab().save(QDir(directory).filePath(name + QStringLiteral(".png"))));
}
template<typename T> T *control(QObject &dialog, const char *name)
{
    auto *result = dialog.findChild<T *>(QString::fromLatin1(name));
    Q_ASSERT(result);
    return result;
}
}

class DialogAudioBackend : public AudioBackend {
public:
    AudioSnapshot current = fixture();
    QVector<AudioRequest> requests;
    bool holdWrite = false, failRead = false;
    quint64 pendingToken = 0;
    AudioRequest pendingRequest;
    int reads = 0;
    void readCurrent(quint64 token) override {
        ++reads;
        emit readFinished(token, !failRead, current,
            failRead ? QStringLiteral("Fixture refresh failed.") : QString());
    }
    void writeChange(quint64 token, const AudioSnapshot &, const AudioRequest &request) override {
        requests << request;
        pendingToken = token; pendingRequest = request;
        if (!holdWrite) complete();
    }
    void complete() {
        auto &objects = pendingRequest.kind == AudioObjectKind::Sink ? current.sinks
            : pendingRequest.kind == AudioObjectKind::Source ? current.sources
            : pendingRequest.kind == AudioObjectKind::Card ? current.cards
            : pendingRequest.kind == AudioObjectKind::SinkInput ? current.sinkInputs : current.sourceOutputs;
        for (auto &object : objects) {
            if (object.index != pendingRequest.index) continue;
            switch (pendingRequest.action) {
            case AudioAction::SetDefault:
                (object.kind == AudioObjectKind::Sink ? current.defaultSink : current.defaultSource) = object.name; break;
            case AudioAction::SetVolume: object.channelVolumes = AudioTypes::scaledVolumes(object, pendingRequest.volumePercent); break;
            case AudioAction::SetChannelVolumes: object.channelVolumes = pendingRequest.channelVolumes; break;
            case AudioAction::SetMute: object.mute = pendingRequest.mute; break;
            case AudioAction::SetPort: object.activePort = pendingRequest.choice; break;
            case AudioAction::SetProfile: object.activeProfile = pendingRequest.choice; break;
            case AudioAction::MoveStream: object.hasRoute = true; object.routeIndex = pendingRequest.destinationIndex; break;
            }
        }
        emit writeFinished(pendingToken, AudioOutcome::Success, current, {});
    }
};

class AudioDialogTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR")) Aero::registerStylesheet(qApp);
    }
    void unavailableAndUnsupportedTabsAreTruthful()
    {
        auto *backend = new DialogAudioBackend;
        backend->current = {};
        backend->current.unavailableReason = QStringLiteral("No PulseAudio-compatible server is available.");
        AudioSettings session(backend);
        SoundDialog dialog(&session);
        QTRY_COMPARE(backend->reads, 1);
        QVERIFY(!session.isReady());
        QVERIFY(control<QLabel>(dialog, "audioStatus")->text().contains(QStringLiteral("unavailable")));
        QVERIFY(!control<QPushButton>(dialog, "playbackSetDefault")->isEnabled());
        QVERIFY(!control<QSpinBox>(dialog, "recordingVolume")->isEnabled());
        auto *buttons = control<QDialogButtonBox>(dialog, "audioDialogButtons");
        QCOMPARE(buttons->standardButtons(), QDialogButtonBox::StandardButtons(QDialogButtonBox::Close));
        for (const auto tab : {SoundDialog::TabSounds, SoundDialog::TabCommunications}) {
            dialog.showTab(tab);
            auto *page = control<QTabWidget>(dialog, "soundTabs")->currentWidget();
            QVERIFY(page->findChildren<QPushButton *>().isEmpty());
            QVERIFY(page->findChildren<QCheckBox *>().isEmpty());
            QVERIFY(page->findChildren<QComboBox *>().isEmpty());
        }
        QVERIFY(dialog.findChildren<QProcess *>().isEmpty());
        QCOMPARE(backend->requests.size(), 0);
        capture(dialog, QStringLiteral("audio-unavailable"));
    }
    void endpointDefaultVolumeMuteAndKeyboardSelection()
    {
        auto *backend = new DialogAudioBackend;
        AudioSettings session(backend);
        SoundDialog dialog(&session);
        QTRY_VERIFY(session.isReady());
        auto *list = control<QListWidget>(dialog, "playbackDevices");
        QCOMPARE(list->currentRow(), 0);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("Default device")));
        dialog.show(); list->setFocus();
        QTest::keyClick(list, Qt::Key_Down);
        QCOMPARE(list->currentRow(), 1);
        control<QPushButton>(dialog, "playbackSetDefault")->click();
        QCOMPARE(backend->requests.last().action, AudioAction::SetDefault);
        QCOMPARE(backend->current.defaultSink, QStringLiteral("output-1"));
        control<QSpinBox>(dialog, "playbackVolume")->setValue(80);
        control<QPushButton>(dialog, "playbackSetVolume")->click();
        QCOMPARE(backend->requests.last().action, AudioAction::SetVolume);
        QCOMPARE(backend->requests.last().volumePercent, 80);
        QCOMPARE(backend->current.sinks[1].channelVolumes, QVector<quint32>({52429, 26215}));
        control<QCheckBox>(dialog, "playbackMute")->click();
        QVERIFY(backend->current.sinks[1].mute);
        dialog.showTab(SoundDialog::TabRecording);
        control<QCheckBox>(dialog, "recordingMute")->click();
        QCOMPARE(backend->requests.last().kind, AudioObjectKind::Source);
        QVERIFY(backend->current.sources[0].mute);
        QVERIFY(dialog.findChildren<QProcess *>().isEmpty());
        capture(dialog, QStringLiteral("audio-recording"));
        dialog.showTab(SoundDialog::TabPlayback);
        capture(dialog, QStringLiteral("audio-playback"));
    }
    void propertiesProvidePortsProfilesAndChannelBalance()
    {
        auto *backend = new DialogAudioBackend;
        AudioSettings session(backend); SoundDialog dialog(&session);
        QTRY_VERIFY(session.isReady());
        control<QPushButton>(dialog, "playbackProperties")->click();
        auto *properties = control<QDialog>(dialog, "audioDeviceProperties");
        auto *port = control<QComboBox>(*properties, "audioPort");
        port->setCurrentIndex(port->findData(QStringLiteral("headphones")));
        control<QPushButton>(*properties, "audioSetPort")->click();
        QCOMPARE(backend->requests.last().action, AudioAction::SetPort);
        QCOMPARE(backend->current.sinks[0].activePort, QStringLiteral("headphones"));
        auto *profile = control<QComboBox>(*properties, "audioProfile");
        profile->setCurrentIndex(profile->findData(QStringLiteral("surround")));
        control<QPushButton>(*properties, "audioSetProfile")->click();
        QCOMPARE(backend->requests.last().kind, AudioObjectKind::Card);
        QCOMPARE(backend->current.cards[0].activeProfile, QStringLiteral("surround"));
        control<QSpinBox>(*properties, "audioChannel0")->setValue(75);
        control<QSpinBox>(*properties, "audioChannel1")->setValue(25);
        control<QPushButton>(*properties, "audioSetChannelLevels")->click();
        QCOMPARE(backend->requests.last().action, AudioAction::SetChannelVolumes);
        QCOMPARE(backend->current.sinks[0].channelVolumes, QVector<quint32>({49152, 16384}));
        capture(*properties, QStringLiteral("audio-properties"));
        backend->current.sinks.removeFirst(); session.refresh();
        QVERIFY(control<QLabel>(*properties, "audioPropertiesStatus")->text().contains(QStringLiteral("no longer available")));
        QVERIFY(!control<QPushButton>(*properties, "audioSetPort")->isEnabled());
        QVERIFY(!control<QPushButton>(*properties, "audioSetChannelLevels")->isEnabled());
    }
    void applicationsCanChangeLevelsMuteAndPlaybackOrRecordingRoutes()
    {
        auto *backend = new DialogAudioBackend;
        AudioSettings session(backend); SoundDialog dialog(&session);
        QTRY_VERIFY(session.isReady()); dialog.showTab(SoundDialog::TabApplications);
        auto *list = control<QListWidget>(dialog, "audioApplications");
        QCOMPARE(list->count(), 2);
        control<QSpinBox>(dialog, "applicationVolume")->setValue(40);
        control<QPushButton>(dialog, "applicationSetVolume")->click();
        QCOMPARE(backend->requests.last().kind, AudioObjectKind::SinkInput);
        control<QCheckBox>(dialog, "applicationMute")->click();
        QVERIFY(backend->current.sinkInputs[0].mute);
        auto *device = control<QComboBox>(dialog, "applicationDevice");
        device->setCurrentIndex(1);
        control<QPushButton>(dialog, "applicationMove")->click();
        QCOMPARE(backend->requests.last().destinationIndex, quint32(2));
        QCOMPARE(backend->requests.last().destinationIdentity, QStringLiteral("sink-identity-1"));
        list->setCurrentRow(1);
        device->setCurrentIndex(1);
        control<QPushButton>(dialog, "applicationMove")->click();
        QCOMPARE(backend->requests.last().kind, AudioObjectKind::SourceOutput);
        QCOMPARE(backend->requests.last().destinationIndex, quint32(4));
        capture(dialog, QStringLiteral("audio-applications"));
    }
    void staleReadsAndDisappearedSelectionsCannotRedirectActions()
    {
        auto *backend = new DialogAudioBackend;
        AudioSettings session(backend); SoundDialog dialog(&session);
        QTRY_VERIFY(session.isReady());
        auto *list = control<QListWidget>(dialog, "playbackDevices");
        list->setCurrentRow(1);
        std::swap(backend->current.sinks[0], backend->current.sinks[1]);
        session.refresh();
        QCOMPARE(list->currentRow(), 0);
        backend->current.sinks.removeFirst(); session.refresh();
        QCOMPARE(list->currentRow(), -1);
        QVERIFY(!control<QPushButton>(dialog, "playbackSetVolume")->isEnabled());
        QVERIFY(control<QLabel>(dialog, "playbackDetails")->text().contains(QStringLiteral("no longer available")));
        backend->failRead = true; session.refresh();
        QVERIFY(control<QLabel>(dialog, "audioStatus")->text().contains(QStringLiteral("Fixture refresh failed")));
        QVERIFY(control<QLabel>(dialog, "audioStatus")->text().contains(QStringLiteral("stale")));
        QVERIFY(!control<QCheckBox>(dialog, "recordingMute")->isEnabled());
        QCOMPARE(backend->requests.size(), 0);
    }
    void retainedJobSurvivesClosingAndRecreatingDialog()
    {
        auto *backend = new DialogAudioBackend;
        AudioSettings session(backend);
        auto *dialog = new SoundDialog(&session);
        QTRY_VERIFY(session.isReady());
        backend->holdWrite = true;
        control<QSpinBox>(*dialog, "playbackVolume")->setValue(65);
        control<QPushButton>(*dialog, "playbackSetVolume")->click();
        QVERIFY(session.isBusy());
        dialog->close(); delete dialog;
        SoundDialog recreated(&session);
        QVERIFY(!control<QPushButton>(recreated, "playbackSetVolume")->isEnabled());
        QVERIFY(control<QLabel>(recreated, "audioStatus")->text().contains(QStringLiteral("busy")));
        backend->complete();
        QVERIFY(!session.isBusy());
        QCOMPARE(control<QSpinBox>(recreated, "playbackVolume")->value(), 65);
        QVERIFY(control<QLabel>(recreated, "audioStatus")->text().contains(QStringLiteral("applied and verified")));
        QCOMPARE(backend->requests.size(), 1);
    }
    void refreshPreservesUnsentEditorsAndAdoptsActualChanges()
    {
        auto *backend = new DialogAudioBackend;
        backend->current.cards[0].profiles << AudioChoice{QStringLiteral("mono"), QStringLiteral("Mono"), QStringLiteral("yes")};
        AudioSettings session(backend); SoundDialog dialog(&session);
        QTRY_VERIFY(session.isReady());
        auto *volume = control<QSpinBox>(dialog, "playbackVolume");
        volume->setValue(78);
        control<QPushButton>(dialog, "playbackProperties")->click();
        auto *properties = control<QDialog>(dialog, "audioDeviceProperties");
        auto *port = control<QComboBox>(*properties, "audioPort");
        auto *profile = control<QComboBox>(*properties, "audioProfile");
        port->setCurrentIndex(port->findData(QStringLiteral("headphones")));
        profile->setCurrentIndex(profile->findData(QStringLiteral("surround")));
        control<QSpinBox>(*properties, "audioChannel0")->setValue(70);
        control<QSpinBox>(*properties, "audioChannel1")->setValue(20);
        auto *appVolume = control<QSpinBox>(dialog, "applicationVolume");
        appVolume->setValue(45);
        auto *route = control<QComboBox>(dialog, "applicationDevice");
        route->setCurrentIndex(1);
        const QString draftRoute = route->currentData().toString();
        const int previousReads = backend->reads;
        emit backend->externalChange();
        QCOMPARE(backend->reads, previousReads + 1);
        QCOMPARE(volume->value(), 78);
        QCOMPARE(appVolume->value(), 45);
        QCOMPARE(port->currentData().toString(), QStringLiteral("headphones"));
        QCOMPARE(profile->currentData().toString(), QStringLiteral("surround"));
        QCOMPARE(control<QSpinBox>(*properties, "audioChannel0")->value(), 70);
        QCOMPARE(control<QSpinBox>(*properties, "audioChannel1")->value(), 20);
        QCOMPARE(route->currentData().toString(), draftRoute);
        QCOMPARE(backend->requests.size(), 0);
        control<QPushButton>(dialog, "playbackSetVolume")->click();
        QCOMPARE(backend->requests.last().volumePercent, 78);
        QCOMPARE(volume->value(), 78);
        // Port/profile drafts survive an unrelated confirmed volume write.
        QCOMPARE(port->currentData().toString(), QStringLiteral("headphones"));
        QCOMPARE(profile->currentData().toString(), QStringLiteral("surround"));
        backend->current.sinks[0].channelVolumes = {58982, 39322};
        backend->current.sinks[0].activePort = QStringLiteral("unplugged");
        backend->current.cards[0].activeProfile = QStringLiteral("mono");
        backend->current.sinkInputs[0].channelVolumes = {42598, 42598};
        session.refresh();
        QCOMPARE(volume->value(), 90);
        QCOMPARE(appVolume->value(), 65);
        QCOMPARE(port->currentData().toString(), QStringLiteral("unplugged"));
        QCOMPARE(profile->currentData().toString(), QStringLiteral("mono"));
        QCOMPARE(control<QSpinBox>(*properties, "audioChannel0")->value(), 90);
        QCOMPARE(control<QSpinBox>(*properties, "audioChannel1")->value(), 60);
        volume->setValue(32);
        auto *devices = control<QListWidget>(dialog, "playbackDevices");
        devices->setCurrentRow(1);
        QCOMPARE(volume->value(), 100);
        devices->setCurrentRow(0);
        QCOMPARE(volume->value(), 90);
    }
    void missingLifetimeIdentitiesRemainVisibleAndReadOnly()
    {
        auto *backend = new DialogAudioBackend;
        backend->current.sinks[0].identity.clear();
        backend->current.sources[0].identity.clear();
        backend->current.sinkInputs[0].identity.clear();
        backend->current.cards[0].identity.clear();
        backend->current.sinks[0].description = QStringLiteral("<b>Speakers</b>");
        AudioSettings session(backend); SoundDialog dialog(&session);
        QTRY_VERIFY(session.isReady());
        QCOMPARE(control<QListWidget>(dialog, "playbackDevices")->count(), 2);
        QVERIFY(control<QListWidget>(dialog, "playbackDevices")->item(0)->text().contains(QStringLiteral("Read-only")));
        QVERIFY(control<QLabel>(dialog, "playbackDetails")->text().contains(QStringLiteral("lifetime identity")));
        QCOMPARE(control<QLabel>(dialog, "playbackDetails")->textFormat(), Qt::PlainText);
        QCOMPARE(control<QLabel>(dialog, "audioStatus")->textFormat(), Qt::PlainText);
        for (const char *name : {"playbackSetDefault", "playbackSetVolume", "playbackProperties", "recordingSetVolume", "applicationSetVolume", "applicationMove"})
            QVERIFY2(!control<QPushButton>(dialog, name)->isEnabled(), name);
        QVERIFY(!control<QCheckBox>(dialog, "playbackMute")->isEnabled());
        QVERIFY(!control<QCheckBox>(dialog, "recordingMute")->isEnabled());
        QVERIFY(!control<QCheckBox>(dialog, "applicationMute")->isEnabled());
        control<QListWidget>(dialog, "playbackDevices")->setCurrentRow(1);
        control<QPushButton>(dialog, "playbackProperties")->click();
        auto *properties = control<QDialog>(dialog, "audioDeviceProperties");
        QVERIFY(!control<QComboBox>(*properties, "audioProfile")->isEnabled());
        QVERIFY(!control<QPushButton>(*properties, "audioSetProfile")->isEnabled());
        QVERIFY(control<QLabel>(*properties, "audioPropertiesStatus")->text().contains(QStringLiteral("Card profiles are read-only")));
        QCOMPARE(control<QLabel>(*properties, "audioPropertiesStatus")->textFormat(), Qt::PlainText);
        // A valid recording stream cannot be moved to an unidentified source.
        control<QListWidget>(dialog, "audioApplications")->setCurrentRow(1);
        control<QComboBox>(dialog, "applicationDevice")->setCurrentIndex(0);
        QVERIFY(!control<QPushButton>(dialog, "applicationMove")->isEnabled());
        QCOMPARE(backend->requests.size(), 0);
    }
    void lightControlsRemainVisibleOnDarkHost()
    {
        const QPalette original = qApp->palette();
        QPalette dark = original;
        dark.setColor(QPalette::Window, QColor("#202020"));
        dark.setColor(QPalette::WindowText, Qt::white);
        dark.setColor(QPalette::Base, QColor("#303030"));
        dark.setColor(QPalette::Text, Qt::white);
        dark.setColor(QPalette::Button, QColor("#303030"));
        dark.setColor(QPalette::ButtonText, Qt::white);
        qApp->setPalette(dark);
        auto *backend = new DialogAudioBackend;
        AudioSettings session(backend); SoundDialog dialog(&session);
        QTRY_VERIFY(session.isReady());
        QVERIFY(dialog.styleSheet().contains(QStringLiteral("color: #000000")));
        QVERIFY(dialog.styleSheet().contains(QStringLiteral("background: #FFFFFF")));
        capture(dialog, QStringLiteral("audio-playback-dark-host"));
        qApp->setPalette(original);
    }
    void controlsHaveAccessibleNamesBuddiesAndReadOnlyCapabilities()
    {
        auto *backend = new DialogAudioBackend;
        backend->current.sources[0].volumeWritable = false;
        backend->current.sources[0].muteWritable = false;
        AudioSettings session(backend); SoundDialog dialog(&session);
        QTRY_VERIFY(session.isReady());
        QVERIFY(!control<QSpinBox>(dialog, "recordingVolume")->isEnabled());
        QVERIFY(!control<QCheckBox>(dialog, "recordingMute")->isEnabled());
        for (auto *widget : dialog.findChildren<QWidget *>()) {
            if (qobject_cast<QSpinBox *>(widget) || qobject_cast<QComboBox *>(widget)
                || qobject_cast<QListWidget *>(widget) || qobject_cast<QCheckBox *>(widget)) {
                QVERIFY2(!widget->accessibleName().isEmpty(), qPrintable(widget->objectName()));
                QVERIFY(QAccessible::queryAccessibleInterface(widget));
                QVERIFY(widget->focusPolicy() != Qt::NoFocus);
            }
        }
        for (auto *spin : dialog.findChildren<QSpinBox *>()) {
            bool buddy = false;
            for (auto *label : dialog.findChildren<QLabel *>()) if (label->buddy() == spin) buddy = true;
            QVERIFY(buddy);
        }
        QCOMPARE(backend->requests.size(), 0);
    }
};

QTEST_MAIN(AudioDialogTest)
#include "audio_dialog_test.moc"
