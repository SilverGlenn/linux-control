#include "SoundDialog.h"
#include "IconHelper.h"
#include "Win7Ui.h"
#include "settings/AudioSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QVBoxLayout>
#include <memory>

namespace {
QString objectKey(const AudioObject &object)
{
    return QStringLiteral("%1/%2/%3").arg(int(object.kind)).arg(object.index).arg(object.identity);
}
QString audioDisplayName(const AudioObject &object)
{
    return object.description.isEmpty() ? object.name : object.description;
}
AudioRequest requestFor(const AudioObject &object, AudioAction action)
{
    AudioRequest request;
    request.action = action;
    request.kind = object.kind;
    request.index = object.index;
    request.identity = object.identity;
    request.name = object.name;
    return request;
}
bool playbackKind(AudioObjectKind kind)
{
    return kind == AudioObjectKind::Sink || kind == AudioObjectKind::SinkInput;
}
void nameControl(QWidget *widget, const QString &name, const QString &accessible)
{
    widget->setObjectName(name);
    widget->setAccessibleName(accessible);
}
void noDefaultButton(QPushButton *button)
{
    button->setAutoDefault(false);
    button->setDefault(false);
}
struct PropertyEditorState {
    QString port, profile, cardIdentity;
    QVector<quint32> channelVolumes;
    QStringList channelNames;
    bool initialized = false;
};
bool selectedChoiceAvailable(const QComboBox *combo)
{
    return combo->currentIndex() >= 0
        && (combo->model()->flags(combo->model()->index(combo->currentIndex(), 0)) & Qt::ItemIsEnabled);
}
void populateChoices(QComboBox *combo, const QVector<AudioChoice> &choices, const QString &active)
{
    combo->clear();
    for (const auto &choice : choices) {
        QString text = choice.description.isEmpty() ? choice.name : choice.description;
        if (!choice.availability.isEmpty())
            text += QStringLiteral(" (%1)").arg(choice.availability);
        combo->addItem(text, choice.name);
        const auto availability = choice.availability.toLower();
        if (availability == QStringLiteral("no") || availability == QStringLiteral("unavailable")
            || availability == QStringLiteral("not available")) {
            if (auto *model = qobject_cast<QStandardItemModel *>(combo->model()))
                model->item(combo->count() - 1)->setEnabled(false);
        }
    }
    combo->setCurrentIndex(combo->findData(active));
}
}

SoundDialog::SoundDialog(QWidget *parent)
    : SoundDialog(AudioSettings::instance(), parent)
{
}

SoundDialog::SoundDialog(AudioSettings *settings, QWidget *parent)
    : QDialog(parent), m_settings(settings)
{
    setWindowTitle(QStringLiteral("Sound"));
    setWindowIcon(themeIcon({"preferences-desktop-sound", "audio-card"}));
    setModal(true);
    Win7::setPointSize(this, 9);
    Win7::styleSettingsDialog(this);
    setStyleSheet(styleSheet() + QStringLiteral(
        " QTabWidget::pane { border: 1px solid #898C95; background: #FFFFFF; }"
        " QTabBar::tab { color: #000000; background: #EAEAEA; border: 1px solid #898C95; padding: 5px 10px; }"
        " QTabBar::tab:selected { background: #FFFFFF; border-bottom-color: #FFFFFF; }"
        " QListWidget:focus { border: 1px solid #3C7FB1; }"
        " QCheckBox { color: #000000; background: transparent; }"
        " QCheckBox:disabled { color: #777777; }"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(9, 9, 9, 9);
    root->setSpacing(8);
    m_tabs = new QTabWidget;
    nameControl(m_tabs, QStringLiteral("soundTabs"), QStringLiteral("Sound settings categories"));
    m_tabs->addTab(buildDeviceTab(m_playback, true), QStringLiteral("&Playback"));
    m_tabs->addTab(buildDeviceTab(m_recording, false), QStringLiteral("&Recording"));
    m_tabs->addTab(buildUnavailableTab(QStringLiteral(
        "Sound schemes and program event assignments are unavailable. No event-sound provider is connected.")), QStringLiteral("&Sounds"));
    m_tabs->addTab(buildUnavailableTab(QStringLiteral(
        "Automatic volume adjustment during calls is unavailable. No communications-policy provider is connected.")), QStringLiteral("&Communications"));
    m_tabs->addTab(buildApplicationsTab(), QStringLiteral("&Applications"));
    root->addWidget(m_tabs, 1);
    m_status = new QLabel;
    nameControl(m_status, QStringLiteral("audioStatus"), QStringLiteral("Audio service status and action results"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_status);
    auto *note = new QLabel(QStringLiteral("Changes take effect immediately. Closing Sound does not undo changes or cancel an audio job."));
    note->setWordWrap(true);
    root->addWidget(note);
    auto *footer = new QHBoxLayout;
    m_refresh = new QPushButton(QStringLiteral("Re&fresh"));
    nameControl(m_refresh, QStringLiteral("audioRefresh"), QStringLiteral("Refresh audio devices and applications"));
    noDefaultButton(m_refresh);
    footer->addWidget(m_refresh);
    footer->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->setObjectName(QStringLiteral("audioDialogButtons"));
    buttons->button(QDialogButtonBox::Close)->setAccessibleName(QStringLiteral("Close Sound"));
    noDefaultButton(buttons->button(QDialogButtonBox::Close));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    footer->addWidget(buttons);
    root->addLayout(footer);
    connect(m_refresh, &QPushButton::clicked, m_settings, &AudioSettings::refresh);
    connect(m_settings, &AudioSettings::stateChanged, this, &SoundDialog::updateView);
    setMinimumSize(610, 540);
    resize(640, 560);
    updateView();
}

void SoundDialog::showTab(Tab tab)
{
    m_tabs->setCurrentIndex(int(tab));
}

QWidget *SoundDialog::buildUnavailableTab(const QString &text)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 16, 16, 16);
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setAccessibleName(text);
    layout->addWidget(label);
    layout->addStretch();
    return page;
}

void SoundDialog::addVolumeEditor(QVBoxLayout *layout, ObjectEditor &editor, const QString &prefix)
{
    auto *row = new QHBoxLayout;
    auto *label = new QLabel(QStringLiteral("&Volume:"));
    editor.volume = new QSpinBox;
    editor.volume->setRange(0, 150);
    editor.volume->setSuffix(QStringLiteral("%"));
    nameControl(editor.volume, prefix + QStringLiteral("Volume"), QStringLiteral("Selected %1 volume percent").arg(prefix));
    label->setBuddy(editor.volume);
    row->addWidget(label);
    row->addWidget(editor.volume);
    editor.setVolume = new QPushButton(QStringLiteral("Set volume &now"));
    nameControl(editor.setVolume, prefix + QStringLiteral("SetVolume"), QStringLiteral("Set selected %1 volume now").arg(prefix));
    noDefaultButton(editor.setVolume);
    row->addWidget(editor.setVolume);
    editor.mute = new QCheckBox(QStringLiteral("&Mute (immediate)"));
    nameControl(editor.mute, prefix + QStringLiteral("Mute"), QStringLiteral("Mute selected %1 immediately").arg(prefix));
    row->addWidget(editor.mute);
    row->addStretch();
    layout->addLayout(row);
    connect(editor.setVolume, &QPushButton::clicked, this, [this, &editor] { setVolume(editor); });
    connect(editor.mute, &QCheckBox::clicked, this, [this, &editor](bool checked) { setMute(editor, checked); });
}

QWidget *SoundDialog::buildDeviceTab(ObjectEditor &editor, bool playback)
{
    const QString prefix = playback ? QStringLiteral("playback") : QStringLiteral("recording");
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 12, 14, 12);
    auto *prompt = new QLabel(playback ? QStringLiteral("Select a &playback device:") : QStringLiteral("Select a &recording device:"));
    layout->addWidget(prompt);
    editor.list = new QListWidget;
    editor.list->setSelectionMode(QAbstractItemView::SingleSelection);
    editor.list->setIconSize(QSize(32, 32));
    editor.list->setSpacing(2);
    nameControl(editor.list, prefix + QStringLiteral("Devices"), playback ? QStringLiteral("Playback devices") : QStringLiteral("Recording devices"));
    prompt->setBuddy(editor.list);
    layout->addWidget(editor.list, 1);
    editor.details = new QLabel;
    editor.details->setTextFormat(Qt::PlainText);
    editor.details->setWordWrap(true);
    nameControl(editor.details, prefix + QStringLiteral("Details"), QStringLiteral("Selected device details"));
    layout->addWidget(editor.details);
    addVolumeEditor(layout, editor, prefix);
    auto *row = new QHBoxLayout;
    row->addStretch();
    editor.setDefault = new QPushButton(QStringLiteral("Set &default now"));
    nameControl(editor.setDefault, prefix + QStringLiteral("SetDefault"), QStringLiteral("Set selected %1 device as default now").arg(prefix));
    noDefaultButton(editor.setDefault);
    row->addWidget(editor.setDefault);
    editor.properties = new QPushButton(QStringLiteral("P&roperties"));
    nameControl(editor.properties, prefix + QStringLiteral("Properties"), QStringLiteral("Selected %1 device properties").arg(prefix));
    noDefaultButton(editor.properties);
    row->addWidget(editor.properties);
    layout->addLayout(row);
    connect(editor.list, &QListWidget::currentRowChanged, this, [this, &editor, playback] {
        editor.selectedId = editor.list->currentItem() ? editor.list->currentItem()->data(Qt::UserRole).toString() : QString();
        updateEditor(editor, playback);
    });
    connect(editor.setDefault, &QPushButton::clicked, this, [this, &editor] { setDefault(editor); });
    connect(editor.properties, &QPushButton::clicked, this, [this, &editor] { showProperties(editor); });
    return page;
}

QWidget *SoundDialog::buildApplicationsTab()
{
    auto &editor = m_applications;
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 12, 14, 12);
    auto *prompt = new QLabel(QStringLiteral("Select an active &application stream:"));
    layout->addWidget(prompt);
    editor.list = new QListWidget;
    editor.list->setSelectionMode(QAbstractItemView::SingleSelection);
    nameControl(editor.list, QStringLiteral("audioApplications"), QStringLiteral("Active playback and recording application streams"));
    prompt->setBuddy(editor.list);
    layout->addWidget(editor.list, 1);
    editor.details = new QLabel;
    editor.details->setTextFormat(Qt::PlainText);
    editor.details->setWordWrap(true);
    nameControl(editor.details, QStringLiteral("applicationDetails"), QStringLiteral("Selected application stream details"));
    layout->addWidget(editor.details);
    addVolumeEditor(layout, editor, QStringLiteral("application"));
    auto *route = new QHBoxLayout;
    auto *label = new QLabel(QStringLiteral("&Device:"));
    editor.route = new QComboBox;
    nameControl(editor.route, QStringLiteral("applicationDevice"), QStringLiteral("Target audio device for selected application stream"));
    label->setBuddy(editor.route);
    route->addWidget(label);
    route->addWidget(editor.route, 1);
    editor.setRoute = new QPushButton(QStringLiteral("&Move now"));
    nameControl(editor.setRoute, QStringLiteral("applicationMove"), QStringLiteral("Move selected application stream to target device now"));
    noDefaultButton(editor.setRoute);
    route->addWidget(editor.setRoute);
    layout->addLayout(route);
    connect(editor.list, &QListWidget::currentRowChanged, this, [this, &editor] {
        editor.selectedId = editor.list->currentItem() ? editor.list->currentItem()->data(Qt::UserRole).toString() : QString();
        updateEditor(editor, true, true);
    });
    connect(editor.setRoute, &QPushButton::clicked, this, [this, &editor] { setRoute(editor); });
    connect(editor.route, &QComboBox::currentIndexChanged, this, [this, &editor] {
        const auto *object = selectedObject(editor);
        editor.setRoute->setEnabled(object && !object->identity.isEmpty() && m_settings->snapshot().available
            && m_settings->snapshot().writable && !m_settings->isBusy() && selectedChoiceAvailable(editor.route));
    });
    return page;
}

const AudioObject *SoundDialog::selectedObject(const ObjectEditor &editor) const
{
    const auto &snapshot = m_settings->snapshot();
    for (const auto kind : { AudioObjectKind::Sink, AudioObjectKind::Source,
                            AudioObjectKind::SinkInput, AudioObjectKind::SourceOutput })
        for (const auto &object : snapshot.objects(kind))
            if (objectKey(object) == editor.selectedId)
                return &object;
    return nullptr;
}

void SoundDialog::updateList(ObjectEditor &editor, int category)
{
    const QSignalBlocker block(editor.list);
    editor.list->clear();
    const auto &snapshot = m_settings->snapshot();
    const auto kinds = category == 0 ? QVector<AudioObjectKind>{AudioObjectKind::Sink}
        : category == 1 ? QVector<AudioObjectKind>{AudioObjectKind::Source}
                        : QVector<AudioObjectKind>{AudioObjectKind::SinkInput, AudioObjectKind::SourceOutput};
    int defaultRow = -1;
    for (const auto kind : kinds) {
        for (const auto &object : snapshot.objects(kind)) {
            const bool isDefault = (kind == AudioObjectKind::Sink && snapshot.defaultSink == object.name)
                || (kind == AudioObjectKind::Source && snapshot.defaultSource == object.name);
            QString status = isDefault ? QStringLiteral("Default device · ") : QString();
            if (category == 2)
                status += playbackKind(kind) ? QStringLiteral("Playback · ") : QStringLiteral("Recording · ");
            status += object.channelVolumes.isEmpty() ? QStringLiteral("Volume unavailable")
                : QStringLiteral("%1% volume").arg(object.volumePercent());
            status += object.mute ? QStringLiteral(" · Muted") : QStringLiteral(" · Not muted");
            if (object.identity.isEmpty()) status += QStringLiteral(" · Read-only: lifetime identity unavailable");
            auto *item = new QListWidgetItem(playbackKind(kind)
                ? themeIcon({"audio-speakers", "audio-card"})
                : themeIcon({"audio-input-microphone", "audio-card"}),
                audioDisplayName(object) + QLatin1Char('\n') + status, editor.list);
            item->setData(Qt::UserRole, objectKey(object));
            item->setToolTip(object.name);
            item->setData(Qt::AccessibleTextRole, audioDisplayName(object) + QStringLiteral(". ") + status);
            item->setSizeHint(QSize(0, 58));
            if (isDefault) defaultRow = editor.list->row(item);
        }
    }
    if (editor.selectedId.isEmpty() && editor.list->count()) {
        auto *initial = editor.list->item(defaultRow < 0 ? 0 : defaultRow);
        editor.selectedId = initial->data(Qt::UserRole).toString();
    }
    for (int row = 0; row < editor.list->count(); ++row)
        if (editor.list->item(row)->data(Qt::UserRole).toString() == editor.selectedId) {
            editor.list->setCurrentRow(row);
            break;
        }
    // Keep a disappeared selection's identity; never silently redirect an edit
    // to a different device or a recycled stream index.
    if (!selectedObject(editor)) editor.list->setCurrentRow(-1);
}

void SoundDialog::updateEditor(ObjectEditor &editor, bool playback, bool applications)
{
    const auto *object = selectedObject(editor);
    const auto &snapshot = m_settings->snapshot();
    const bool sameSelection = object && editor.renderedId == editor.selectedId;
    const bool enabled = object && !object->identity.isEmpty() && snapshot.available && snapshot.writable && !m_settings->isBusy();
    {
        const QSignalBlocker block(editor.mute);
        editor.mute->setChecked(object && object->mute);
    }
    if (!sameSelection || !object || editor.confirmedVolumes != object->channelVolumes)
        editor.volume->setValue(object ? qBound(0, object->volumePercent(), 150) : 0);
    editor.confirmedVolumes = object ? object->channelVolumes : QVector<quint32>();
    editor.volume->setEnabled(enabled && object->volumeWritable && !object->channelVolumes.isEmpty());
    editor.setVolume->setEnabled(editor.volume->isEnabled());
    editor.mute->setEnabled(enabled && object->muteWritable);
    if (!object) {
        editor.details->setText(editor.selectedId.isEmpty()
            ? (applications ? QStringLiteral("No active application streams were reported.")
                            : QStringLiteral("No audio devices were reported."))
            : QStringLiteral("The selected device or stream is no longer available. Select another item or refresh."));
    } else {
        editor.details->setText(audioDisplayName(*object) + QStringLiteral("\n") + object->name
            + (object->identity.isEmpty() ? QStringLiteral("\nRead-only: the audio server did not report a stable lifetime identity.") : QString()));
    }
    if (!applications) {
        const QString currentDefault = playback ? snapshot.defaultSink : snapshot.defaultSource;
        editor.setDefault->setEnabled(enabled && object->name != currentDefault);
        editor.properties->setEnabled(object && !object->identity.isEmpty() && snapshot.available);
    } else {
        const QSignalBlocker block(editor.route);
        const QString draftRoute = editor.route->currentData().toString();
        editor.route->clear();
        QString confirmedRoute;
        if (object) {
            const auto &devices = snapshot.objects(playbackKind(object->kind)
                ? AudioObjectKind::Sink : AudioObjectKind::Source);
            QString actualRoute;
            for (const auto &device : devices) {
                editor.route->addItem(audioDisplayName(device), objectKey(device));
                if (device.identity.isEmpty()) {
                    if (auto *model = qobject_cast<QStandardItemModel *>(editor.route->model())) {
                        auto *item = model->item(editor.route->count() - 1);
                        item->setEnabled(false);
                        item->setToolTip(QStringLiteral("Read-only: the destination lifetime identity is unavailable."));
                    }
                }
                if (object->hasRoute && device.index == object->routeIndex)
                    actualRoute = objectKey(device);
            }
            confirmedRoute = QStringLiteral("%1/%2/%3").arg(object->hasRoute ? 1 : 0).arg(object->routeIndex).arg(actualRoute);
            const QString selectedRoute = sameSelection && editor.confirmedRoute == confirmedRoute ? draftRoute : actualRoute;
            editor.route->setCurrentIndex(editor.route->findData(selectedRoute));
        }
        editor.confirmedRoute = confirmedRoute;
        editor.route->setEnabled(enabled && editor.route->count() > 0);
        editor.setRoute->setEnabled(editor.route->isEnabled() && selectedChoiceAvailable(editor.route));
    }
    editor.renderedId = object ? editor.selectedId : QString();
}

void SoundDialog::updateView()
{
    updateList(m_playback, 0);
    updateList(m_recording, 1);
    updateList(m_applications, 2);
    updateEditor(m_playback, true);
    updateEditor(m_recording, false);
    updateEditor(m_applications, true, true);
    const auto &snapshot = m_settings->snapshot();
    const bool hasReportedObjects = !snapshot.sinks.isEmpty() || !snapshot.sources.isEmpty()
        || !snapshot.sinkInputs.isEmpty() || !snapshot.sourceOutputs.isEmpty();
    QString status;
    if (m_settings->isBusy()) {
        status = hasReportedObjects
            ? QStringLiteral("Audio service busy. Showing the last confirmed values; changes are temporarily disabled.")
            : QStringLiteral("Reading audio service information. Changes are temporarily disabled.");
    } else if (!m_settings->errorText().isEmpty()) {
        status = m_settings->errorText();
        if (hasReportedObjects)
            status += QStringLiteral(" Last reported device information may be stale. Refresh to verify it.");
    } else if (!snapshot.available) {
        status = QStringLiteral("Audio service unavailable.");
    } else {
        status = QStringLiteral("Audio service: %1. Values reflect the latest completed refresh.")
            .arg(snapshot.serverName.isEmpty() ? QStringLiteral("connected") : snapshot.serverName);
    }
    if (!m_settings->supportText().isEmpty())
        status += QLatin1Char(' ') + m_settings->supportText();
    if (!m_settings->resultText().isEmpty())
        status += QLatin1Char(' ') + m_settings->resultText();
    m_status->setText(status);
    m_refresh->setEnabled(!m_settings->isBusy());
}

void SoundDialog::setVolume(ObjectEditor &editor)
{
    if (const auto *object = selectedObject(editor); object && !object->identity.isEmpty()) {
        auto request = requestFor(*object, AudioAction::SetVolume);
        request.volumePercent = editor.volume->value();
        m_settings->requestChange(request);
    }
}
void SoundDialog::setMute(ObjectEditor &editor, bool mute)
{
    if (const auto *object = selectedObject(editor); object && !object->identity.isEmpty()) {
        auto request = requestFor(*object, AudioAction::SetMute);
        request.mute = mute;
        m_settings->requestChange(request);
    }
}
void SoundDialog::setDefault(ObjectEditor &editor)
{
    if (const auto *object = selectedObject(editor); object && !object->identity.isEmpty())
        m_settings->requestChange(requestFor(*object, AudioAction::SetDefault));
}
void SoundDialog::setRoute(ObjectEditor &editor)
{
    const auto *object = selectedObject(editor);
    if (!object || object->identity.isEmpty()) return;
    const QString target = editor.route->currentData().toString();
    const auto &devices = m_settings->snapshot().objects(playbackKind(object->kind)
        ? AudioObjectKind::Sink : AudioObjectKind::Source);
    for (const auto &device : devices)
        if (objectKey(device) == target && !device.identity.isEmpty()) {
            auto request = requestFor(*object, AudioAction::MoveStream);
            request.destinationIndex = device.index;
            request.destinationIdentity = device.identity;
            m_settings->requestChange(request);
            return;
        }
}

void SoundDialog::showProperties(ObjectEditor &editor)
{
    const auto *selected = selectedObject(editor);
    if (!selected || selected->identity.isEmpty()) return;
    const QString identity = objectKey(*selected);
    const auto kind = selected->kind;
    const quint32 index = selected->index;
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("audioDeviceProperties"));
    dialog->setWindowTitle(QStringLiteral("%1 Properties").arg(audioDisplayName(*selected)));
    dialog->setWindowModality(Qt::NonModal);
    Win7::styleSettingsDialog(dialog);
    auto *layout = new QVBoxLayout(dialog);
    auto *status = new QLabel;
    status->setObjectName(QStringLiteral("audioPropertiesStatus"));
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *portRow = new QHBoxLayout;
    auto *port = new QComboBox;
    nameControl(port, QStringLiteral("audioPort"), QStringLiteral("Device port"));
    auto *portLabel = new QLabel(QStringLiteral("&Port:"));
    portLabel->setBuddy(port);
    portRow->addWidget(portLabel);
    portRow->addWidget(port, 1);
    auto *setPort = new QPushButton(QStringLiteral("Change port &now"));
    nameControl(setPort, QStringLiteral("audioSetPort"), QStringLiteral("Change selected device port now"));
    noDefaultButton(setPort);
    portRow->addWidget(setPort);
    layout->addLayout(portRow);
    auto *profileRow = new QHBoxLayout;
    auto *profile = new QComboBox;
    nameControl(profile, QStringLiteral("audioProfile"), QStringLiteral("Audio card profile"));
    auto *profileLabel = new QLabel(QStringLiteral("Card pro&file:"));
    profileLabel->setBuddy(profile);
    profileRow->addWidget(profileLabel);
    profileRow->addWidget(profile, 1);
    auto *setProfile = new QPushButton(QStringLiteral("Change profile no&w"));
    nameControl(setProfile, QStringLiteral("audioSetProfile"), QStringLiteral("Change audio card profile now"));
    noDefaultButton(setProfile);
    profileRow->addWidget(setProfile);
    layout->addLayout(profileRow);
    auto *warning = new QLabel(QStringLiteral("Port and profile changes take effect immediately. A profile change can replace playback and recording devices."));
    warning->setWordWrap(true);
    layout->addWidget(warning);
    auto *levelsTitle = new QLabel(QStringLiteral("Channel levels (balance):"));
    layout->addWidget(levelsTitle);
    auto *levels = new QFormLayout;
    layout->addLayout(levels);
    auto *setLevels = new QPushButton(QStringLiteral("Set channel &levels now"));
    nameControl(setLevels, QStringLiteral("audioSetChannelLevels"), QStringLiteral("Set device channel levels and balance now"));
    noDefaultButton(setLevels);
    layout->addWidget(setLevels, 0, Qt::AlignRight);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    noDefaultButton(buttons->button(QDialogButtonBox::Close));
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    auto channelControls = std::make_shared<QVector<QSpinBox *>>();
    auto confirmed = std::make_shared<PropertyEditorState>();
    const auto currentObject = [this, kind, index, identity]() -> const AudioObject * {
        const auto *object = m_settings->snapshot().object(kind, index);
        return object && objectKey(*object) == identity ? object : nullptr;
    };
    const auto update = [this, dialog, status, port, setPort, profile, setProfile,
                         levels, setLevels, channelControls, currentObject, confirmed] {
        const auto *object = currentObject();
        const auto &snapshot = m_settings->snapshot();
        const bool enabled = object && !object->identity.isEmpty() && snapshot.available && snapshot.writable && !m_settings->isBusy();
        status->setText(object ? audioDisplayName(*object)
            : QStringLiteral("This device is no longer available. Close Properties and select another device."));
        if (m_settings->isBusy()) status->setText(status->text() + QStringLiteral("\nAudio service busy. Showing the last confirmed values."));
        if (!snapshot.available) status->setText(status->text() + QStringLiteral("\nAudio service unavailable."));
        if (!m_settings->errorText().isEmpty()) status->setText(status->text() + QLatin1Char('\n') + m_settings->errorText());
        if (!m_settings->resultText().isEmpty()) status->setText(status->text() + QLatin1Char('\n') + m_settings->resultText());
        if (!m_settings->supportText().isEmpty()) status->setText(status->text() + QLatin1Char('\n') + m_settings->supportText());
        const QSignalBlocker blockPort(port), blockProfile(profile);
        if (object) {
            const QString selectedPort = confirmed->initialized && confirmed->port == object->activePort
                ? port->currentData().toString() : object->activePort;
            populateChoices(port, object->ports, selectedPort);
        } else port->clear();
        port->setToolTip(port->count() ? QStringLiteral("Select a reported device port, then choose Change port now.")
            : QStringLiteral("No selectable ports were reported for this device."));
        port->setEnabled(enabled && port->count() > 0);
        setPort->setEnabled(port->isEnabled() && selectedChoiceAvailable(port));
        const auto *card = object && object->hasCard ? snapshot.object(AudioObjectKind::Card, object->cardIndex) : nullptr;
        if (card) {
            const QString selectedProfile = confirmed->initialized && confirmed->cardIdentity == objectKey(*card)
                && confirmed->profile == card->activeProfile ? profile->currentData().toString() : card->activeProfile;
            populateChoices(profile, card->profiles, selectedProfile);
        } else profile->clear();
        profile->setToolTip(profile->count() ? QStringLiteral("Select a reported sound-card profile, then choose Change profile now.")
            : QStringLiteral("No selectable sound-card profiles were reported for this device."));
        profile->setEnabled(enabled && card && !card->identity.isEmpty() && profile->count() > 0);
        if (card && card->identity.isEmpty()) {
            profile->setToolTip(QStringLiteral("Read-only: the sound card lifetime identity is unavailable."));
            status->setText(status->text() + QStringLiteral("\nCard profiles are read-only: the sound card lifetime identity is unavailable."));
        }
        setProfile->setEnabled(profile->isEnabled() && selectedChoiceAvailable(profile));
        const int count = object ? object->channelVolumes.size() : 0;
        const bool layoutChanged = object && confirmed->channelNames != object->channelNames;
        const bool valuesChanged = !confirmed->initialized || !object || confirmed->channelVolumes != object->channelVolumes;
        if (channelControls->size() != count || layoutChanged) {
            while (levels->rowCount()) levels->removeRow(0);
            channelControls->clear();
            for (int i = 0; i < count; ++i) {
                auto *spin = new QSpinBox(dialog);
                spin->setRange(0, 150);
                spin->setSuffix(QStringLiteral("%"));
                QString channel = object->channelNames.value(i, QStringLiteral("Channel %1").arg(i + 1));
                nameControl(spin, QStringLiteral("audioChannel%1").arg(i), channel + QStringLiteral(" volume percent"));
                auto *label = new QLabel(channel + QLatin1Char(':'));
                label->setBuddy(spin);
                levels->addRow(label, spin);
                channelControls->append(spin);
            }
        }
        for (int i = 0; i < count; ++i) {
            if (valuesChanged || layoutChanged)
                (*channelControls)[i]->setValue(qRound(object->channelVolumes[i] * 100.0 / AudioTypes::NormalVolume));
            (*channelControls)[i]->setEnabled(enabled && object->volumeWritable);
        }
        setLevels->setEnabled(enabled && object->volumeWritable && count > 0);
        confirmed->initialized = object != nullptr;
        confirmed->port = object ? object->activePort : QString();
        confirmed->cardIdentity = card ? objectKey(*card) : QString();
        confirmed->profile = card ? card->activeProfile : QString();
        confirmed->channelVolumes = object ? object->channelVolumes : QVector<quint32>();
        confirmed->channelNames = object ? object->channelNames : QStringList();
    };
    connect(port, &QComboBox::currentIndexChanged, dialog, [this, currentObject, port, setPort] {
        const auto *object = currentObject();
        setPort->setEnabled(object && !object->identity.isEmpty() && m_settings->snapshot().available && m_settings->snapshot().writable
            && !m_settings->isBusy() && selectedChoiceAvailable(port));
    });
    connect(profile, &QComboBox::currentIndexChanged, dialog, [this, currentObject, profile, setProfile] {
        const auto *object = currentObject();
        const auto *card = object && object->hasCard
            ? m_settings->snapshot().object(AudioObjectKind::Card, object->cardIndex) : nullptr;
        setProfile->setEnabled(object && !object->identity.isEmpty() && card && !card->identity.isEmpty()
            && m_settings->snapshot().available && m_settings->snapshot().writable
            && !m_settings->isBusy() && selectedChoiceAvailable(profile));
    });
    connect(m_settings, &AudioSettings::stateChanged, dialog, update);
    connect(setPort, &QPushButton::clicked, dialog, [this, currentObject, port] {
        if (const auto *object = currentObject(); object && !object->identity.isEmpty()) {
            auto request = requestFor(*object, AudioAction::SetPort);
            request.choice = port->currentData().toString();
            m_settings->requestChange(request);
        }
    });
    connect(setProfile, &QPushButton::clicked, dialog, [this, currentObject, profile] {
        const auto *object = currentObject();
        const auto *card = object && object->hasCard
            ? m_settings->snapshot().object(AudioObjectKind::Card, object->cardIndex) : nullptr;
        if (object && !object->identity.isEmpty() && card && !card->identity.isEmpty()) {
            auto request = requestFor(*card, AudioAction::SetProfile);
            request.choice = profile->currentData().toString();
            m_settings->requestChange(request);
        }
    });
    connect(setLevels, &QPushButton::clicked, dialog, [this, currentObject, channelControls] {
        if (const auto *object = currentObject(); object && !object->identity.isEmpty()) {
            auto request = requestFor(*object, AudioAction::SetChannelVolumes);
            for (const auto *spin : *channelControls)
                request.channelVolumes.append(quint32(qRound(spin->value() * AudioTypes::NormalVolume / 100.0)));
            m_settings->requestChange(request);
        }
    });
    update();
    dialog->setMinimumWidth(570);
    dialog->show();
}
