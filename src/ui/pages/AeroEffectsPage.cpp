#include "AeroEffectsPage.h"
#include "settings/AeroEffectSettings.h"
#include "Win7Ui.h"
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>

AeroEffectsPage::AeroEffectsPage(QScrollArea *sidebar, QWidget *parent)
    : AeroEffectsPage(sidebar, AeroEffectSettings::instance(), parent)
{
}

AeroEffectsPage::AeroEffectsPage(QScrollArea *sidebar, AeroEffectSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 710);
    Win7::styleSettingsControls(this);
    layout->addWidget(Win7::pageTitle(tr("Change Aero Glass Blur strength")));
    layout->addSpacing(12);
    layout->addWidget(Win7::bodyLabel(tr("Adjust the background blur used by the installed Aero Glass Blur effect. "
        "Choose a strength, then click Apply to save it.")));
    layout->addSpacing(22);
    layout->addLayout(Win7::sectionHeading(tr("Aero Glass Blur")));
    layout->addSpacing(12);
    auto *row = new QHBoxLayout;
    row->setContentsMargins(12, 0, 0, 0);
    auto *label = Win7::label(tr("&Blur strength:"));
    m_strength = new QComboBox;
    m_strength->setObjectName(QStringLiteral("aeroBlurStrength"));
    m_strength->setAccessibleName(tr("Aero Glass Blur strength"));
    m_strength->setMinimumWidth(220);
    for (int i = 2; i <= 15; ++i)
        m_strength->addItem(i == 2 ? tr("2 — least blur") : i == 15 ? tr("15 — most blur")
                           : i == 3 ? tr("3 — default") : QString::number(i), i);
    label->setBuddy(m_strength);
    row->addWidget(label); row->addWidget(m_strength); row->addStretch();
    layout->addLayout(row);
    layout->addSpacing(18);
    auto *scope = Win7::bodyLabel(tr("This changes one preference inside Aero Glass Blur. "
        "The effect must already be loaded; Apply does not enable or disable desktop effects. "
        "Saved strength is read back after Apply, and KWin's reconfiguration reply and loaded status are checked. "
        "The effect does not expose the blur strength it is rendering."));
    scope->setStyleSheet(QStringLiteral("color: #555555; background: transparent;"));
    layout->addWidget(scope);
    layout->addSpacing(18);
    m_status = Win7::bodyLabel(QString());
    m_status->setObjectName(QStringLiteral("aeroEffectStatus"));
    m_status->setTextFormat(Qt::PlainText); layout->addWidget(m_status);
    m_message = Win7::bodyLabel(QString());
    m_message->setObjectName(QStringLiteral("aeroEffectMessage"));
    m_message->setTextFormat(Qt::PlainText); layout->addWidget(m_message);
    layout->addSpacing(12);
    auto *buttons = new QHBoxLayout; buttons->addStretch();
    m_apply = new QPushButton(tr("&Apply"));
    m_apply->setObjectName(QStringLiteral("applyAeroEffect"));
    m_revert = new QPushButton(tr("&Revert"));
    m_revert->setObjectName(QStringLiteral("revertAeroEffect"));
    buttons->addWidget(m_apply); buttons->addWidget(m_revert); layout->addLayout(buttons);
    layout->addStretch();
    connect(m_strength, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) m_session->selectStrength(m_strength->itemData(index).toInt());
    });
    connect(m_apply, &QPushButton::clicked, session, &AeroEffectSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &AeroEffectSettings::revert);
    connect(session, &SettingsSession::stateChanged, this, &AeroEffectsPage::updateState);
    updateState();
}

SettingsSession *AeroEffectsPage::settingsSession() const { return m_session; }
QList<SidebarLink> AeroEffectsPage::sidebarLinks()
{
    return {Nav::to("Personalization", PageId::Personalization)};
}
QList<SidebarLink> AeroEffectsPage::sidebarSeeAlso()
{
    return {Nav::to("Icons, pointers and application style", PageId::ThemeComponents),
            Nav::to("Window Behavior", PageId::WindowBehavior)};
}

void AeroEffectsPage::updateState()
{
    const QSignalBlocker blocker(m_strength);
    while (m_strength->count() > 14) m_strength->removeItem(14);
    int index = m_strength->findData(m_session->stagedStrength());
    if (index < 0) {
        m_strength->addItem(tr("Current strength: %1 (unsupported)").arg(m_session->stagedStrength()), m_session->stagedStrength());
        index = m_strength->count() - 1;
    }
    m_strength->setCurrentIndex(index);
    m_strength->setEnabled(m_session->canApply() && !m_session->isBusy() && !m_session->hasConflict());
    const auto state = m_session->state();
    QString status = m_session->isReady() ? tr("Saved blur strength: %1").arg(m_session->appliedStrength())
        : tr("Saved blur strength: unavailable");
    if (m_session->isDirty()) status += tr("\nSelected: %1 (not applied)").arg(m_session->stagedStrength());
    status += state.pluginSupported ? tr("\nInstalled effect build: supported") : tr("\nInstalled effect build: unsupported or unavailable");
    status += state.effectLoaded ? tr("\nKWin reports Aero Glass Blur loaded.") : tr("\nAero Glass Blur is not confirmed loaded.");
    if (m_session->isBusy()) status += tr("\nChecking or saving preferences…");
    m_status->setText(status);
    QString message = m_session->errorText();
    if (m_session->hasConflict()) {
        if (!message.isEmpty()) message += QLatin1Char('\n');
        message += tr("Your selection is preserved. Revert reloads the saved preference before another Apply.");
    }
    if (!state.unavailableReason.isEmpty() && !message.contains(state.unavailableReason)) {
        if (!message.isEmpty()) message += QLatin1Char('\n');
        message += state.unavailableReason;
    }
    if (message.isEmpty()) message = m_session->resultText();
    m_message->setText(message); m_message->setVisible(!message.isEmpty());
    m_message->setStyleSheet(m_session->hasConflict() || !m_session->errorText().isEmpty() || !state.unavailableReason.isEmpty()
        ? QStringLiteral("color: #A02C20; background: transparent;")
        : QStringLiteral("color: #333333; background: transparent;"));
    m_apply->setEnabled(m_session->isDirty() && m_session->canApply() && !m_session->isBusy()
        && !m_session->hasConflict() && AeroEffectSettings::supportedStrength(m_session->stagedStrength()));
    m_revert->setEnabled(!m_session->isBusy() && (m_session->isDirty() || m_session->hasConflict()
        || !m_session->isReady() || !m_session->errorText().isEmpty() || !m_session->canApply()));
}
