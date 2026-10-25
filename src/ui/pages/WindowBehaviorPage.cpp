#include "WindowBehaviorPage.h"

#include "Win7Ui.h"
#include "settings/AppearanceSettings.h"
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>

WindowBehaviorPage::WindowBehaviorPage(QScrollArea *sidebar, QWidget *parent)
    : WindowBehaviorPage(sidebar, AppearanceSettings::instance(), parent)
{
}

WindowBehaviorPage::WindowBehaviorPage(QScrollArea *sidebar, AppearanceSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 700);
    Win7::styleSettingsControls(this);
    layout->addWidget(Win7::pageTitle(tr("Change how your windows behave")));
    layout->addSpacing(10);
    auto *intro = Win7::label(tr("Choose what happens when you double-click a window's title bar. "
                                "Click Apply to save your choice."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addSpacing(20);
    layout->addLayout(Win7::sectionHeading(tr("Title bar")));
    layout->addSpacing(12);
    auto *row = new QHBoxLayout;
    row->setContentsMargins(12, 0, 0, 0);
    auto *label = Win7::label(tr("&Double-click action:"));
    m_action = new QComboBox;
    m_action->setObjectName(QStringLiteral("titlebarDoubleClickAction"));
    m_action->setAccessibleName(tr("Title-bar double-click action"));
    m_action->addItem(tr("Maximize or restore the window"), QStringLiteral("Maximize"));
    m_action->addItem(tr("Minimize the window"), QStringLiteral("Minimize"));
    m_action->addItem(tr("Do nothing"), QStringLiteral("Nothing"));
    m_action->setMinimumWidth(275);
    label->setBuddy(m_action);
    row->addWidget(label);
    row->addWidget(m_action);
    row->addStretch();
    layout->addLayout(row);
    layout->addSpacing(16);
    auto *scope = Win7::label(tr("This preference controls title bars managed by KDE's window manager. "
                                "Some applications draw their own title bars and may handle double-clicks differently."), 9, "#555555");
    scope->setWordWrap(true);
    layout->addWidget(scope);
    layout->addSpacing(24);
    m_status = Win7::label(QString());
    m_status->setObjectName(QStringLiteral("windowBehaviorStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_message = Win7::label(QString(), 9, "#A02C20");
    m_message->setObjectName(QStringLiteral("windowBehaviorMessage"));
    m_message->setTextFormat(Qt::PlainText);
    m_message->setWordWrap(true);
    layout->addWidget(m_message);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    m_apply = new QPushButton(tr("&Apply"));
    m_apply->setObjectName(QStringLiteral("applyWindowBehavior"));
    m_revert = new QPushButton(tr("&Revert"));
    m_revert->setObjectName(QStringLiteral("revertWindowBehavior"));
    buttons->addWidget(m_apply);
    buttons->addWidget(m_revert);
    layout->addLayout(buttons);
    layout->addStretch();
    connect(m_action, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_session->selectAction(m_action->itemData(index).toString());
    });
    connect(m_apply, &QPushButton::clicked, session, &AppearanceSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &AppearanceSettings::revert);
    connect(session, &SettingsSession::stateChanged, this, &WindowBehaviorPage::updateState);
    updateState();
}

SettingsSession *WindowBehaviorPage::settingsSession() const
{
    return m_session;
}

QList<SidebarLink> WindowBehaviorPage::sidebarLinks()
{
    return {Nav::to("Personalization", PageId::Personalization)};
}

QList<SidebarLink> WindowBehaviorPage::sidebarSeeAlso()
{
    return {Nav::to("Ease of Access Center", PageId::EaseOfAccess), Nav::to("Aero Effects", PageId::AeroEffects)};
}

void WindowBehaviorPage::updateState()
{
    const QSignalBlocker blocker(m_action);
    // Retain any existing KDE action rather than coercing it to our three choices.
    while (m_action->count() > 3)
        m_action->removeItem(3);
    int index = m_action->findData(m_session->stagedAction());
    if (index < 0 && !m_session->stagedAction().isEmpty()) {
        m_action->addItem(tr("Current KDE action: %1").arg(m_session->stagedAction()), m_session->stagedAction());
        index = m_action->count() - 1;
    }
    m_action->setCurrentIndex(index);
    m_action->setEnabled(m_session->isReady() && !m_session->isBusy() && m_session->canApply());
    const auto name = [this](const QString &action) {
        const int item = m_action->findData(action);
        return item >= 0 ? m_action->itemText(item) : action;
    };
    QString status = m_session->isReady()
        ? tr("Saved setting: %1").arg(name(m_session->appliedAction()))
        : tr("Saved setting: unavailable");
    if (m_session->isDirty())
        status += tr("\nSelected: %1 (not applied)").arg(name(m_session->stagedAction()));
    if (m_session->isBusy())
        status += QLatin1Char('\n') + (m_session->pendingMessage().isEmpty() ? tr("Working…") : m_session->pendingMessage());
    m_status->setText(status);
    QString message = m_session->errorText();
    if (m_session->hasConflict()) {
        if (!message.isEmpty())
            message += QLatin1Char('\n');
        message += tr("Your selection is preserved. Revert reloads the saved setting before another Apply.");
    }
    if (!m_session->canApply() && !m_session->unavailableReason().isEmpty()) {
        if (!message.isEmpty())
            message += QLatin1Char('\n');
        message += m_session->unavailableReason();
    }
    if (message.isEmpty())
        message = m_session->resultText();
    const bool problem = !m_session->errorText().isEmpty() || m_session->hasConflict()
        || (!m_session->canApply() && !m_session->unavailableReason().isEmpty());
    m_message->setStyleSheet(problem ? QStringLiteral("color: #A02C20; background: transparent;")
                                   : QStringLiteral("color: #333333; background: transparent;"));
    m_message->setText(message);
    m_message->setVisible(!message.isEmpty());
    m_apply->setEnabled(m_session->isDirty() && m_session->canApply()
                        && !m_session->isBusy() && !m_session->hasConflict());
    m_revert->setEnabled(!m_session->isBusy() && (m_session->isDirty() || m_session->hasConflict()
                        || !m_session->isReady() || !m_session->errorText().isEmpty()));
}
