#include "ThemeComponentsPage.h"
#include "Win7Ui.h"
#include "LinkLabel.h"
#include "settings/AeroThemeSettings.h"
#include "settings/AeroThemeContract.h"
#include <QLabel>
#include <QPushButton>

ThemeComponentsPage::ThemeComponentsPage(QScrollArea *sidebar, QWidget *parent)
    : ThemeComponentsPage(sidebar, AeroThemeSettings::instance(), parent) {}
ThemeComponentsPage::ThemeComponentsPage(QScrollArea *sidebar, AeroThemeSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 760);
    layout->addWidget(Win7::pageTitle(tr("Custom Aero component status"))); layout->addSpacing(10);
    layout->addWidget(Win7::bodyLabel(tr("These coordinated components belong to the custom Windows 7 Aero theme. "
        "Saved settings are compared below. Personalization restores the approved components when you apply a customization.")));
    layout->addSpacing(16); layout->addLayout(Win7::sectionHeading(tr("Approved and saved components"))); layout->addSpacing(10);
    const auto identities = AeroThemeContract::identities();
    for (const auto &field : AeroThemeContract::fields()) {
        if (!identities.contains(field.key)) continue;
        auto *label = Win7::bodyLabel(QString());
        label->setTextFormat(Qt::PlainText); label->setObjectName(field.key + QStringLiteral("Status"));
        label->setAccessibleName(field.label); layout->addWidget(label); layout->addSpacing(8);
        m_components.insert(field.key, label);
    }
    auto *customize = new LinkLabel(tr("Change window colors, glass, pointers, and borders"));
    connect(customize, &LinkLabel::clicked, this, [this] { emit navigationRequested(PageId::Personalization); });
    layout->addSpacing(10); layout->addWidget(customize); layout->addSpacing(16);
    layout->addWidget(Win7::bodyLabel(tr("Restore Aero defaults stages the approved components, colors, glass options, pointer size, and supported SMOD options. "
        "Wallpaper, fonts, panel layout, and shortcuts are preserved. Review the changes, then click Apply to save.")));
    m_restore = new QPushButton(tr("Restore Aero &defaults")); m_restore->setObjectName(QStringLiteral("restoreAeroDefaults"));
    layout->addWidget(m_restore, 0, Qt::AlignLeft); layout->addSpacing(12);
    m_review = Win7::bodyLabel(QString()); m_review->setObjectName(QStringLiteral("aeroComponentsReview")); m_review->setTextFormat(Qt::PlainText); layout->addWidget(m_review);
    m_message = Win7::bodyLabel(QString()); m_message->setObjectName(QStringLiteral("aeroComponentsMessage")); m_message->setTextFormat(Qt::PlainText); layout->addWidget(m_message);
    auto *actions = new QHBoxLayout; actions->addStretch();
    m_apply = new QPushButton(tr("&Apply")); m_apply->setObjectName(QStringLiteral("applyAeroTheme"));
    m_revert = new QPushButton(tr("&Revert")); m_revert->setObjectName(QStringLiteral("revertAeroTheme"));
    actions->addWidget(m_apply); actions->addWidget(m_revert); layout->addLayout(actions); layout->addStretch();
    connect(m_restore, &QPushButton::clicked, session, &AeroThemeSettings::stageRestoreDefaults);
    connect(m_apply, &QPushButton::clicked, session, &AeroThemeSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &AeroThemeSettings::revert);
    connect(session, &SettingsSession::stateChanged, this, &ThemeComponentsPage::updateState);
    Win7::styleSettingsControls(this); updateState();
}
SettingsSession *ThemeComponentsPage::settingsSession() const { return m_session; }
QList<SidebarLink> ThemeComponentsPage::sidebarLinks() { return {Nav::to("Personalization", PageId::Personalization)}; }
QList<SidebarLink> ThemeComponentsPage::sidebarSeeAlso()
{
    return {Nav::to("Font Settings", PageId::FontSettings), Nav::to("Window Behavior", PageId::WindowBehavior)};
}
void ThemeComponentsPage::updateState()
{
    const auto identities = AeroThemeContract::identities();
    for (const auto &field : AeroThemeContract::fields()) {
        auto *label = m_components.value(field.key); if (!label) continue;
        const bool palette = field.defaultValue.metaType().id() == QMetaType::QVariantMap;
        const QString approved = palette ? tr("Versioned Aero palette")
            : field.defaultValue.toString().isEmpty() ? tr("Built-in Aero resource") : field.defaultValue.toString();
        const bool matches = m_session->appliedValue(field.key) == identities.value(field.key);
        const QString saved = !m_session->isReady() ? tr("Unavailable") : palette ? (matches ? tr("Approved Aero palette") : tr("Different palette")) : m_session->appliedValue(field.key).toString().isEmpty() ? tr("Built-in Aero resource") : m_session->appliedValue(field.key).toString();
        QString text = tr("%1\nApproved: %2    Saved: %3").arg(field.label, approved, saved);
        if (m_session->isReady()) text += matches ? tr("    Matches") : tr("    Differs from approved configuration");
        if (m_session->stagedValue(field.key) != m_session->appliedValue(field.key)) text += tr("\nSelected: approved Aero component (not applied)");
        label->setText(text);
        label->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(m_session->isReady() && !matches ? QStringLiteral("#A02C20") : QStringLiteral("#000000")));
    }
    const auto changes = m_session->changeSummary();
    m_review->setText(changes.isEmpty() ? QString() : tr("Changes to apply:\n%1").arg(changes.join(QLatin1Char('\n')))); m_review->setVisible(!changes.isEmpty());
    QStringList messages = m_session->state().issues;
    if (m_session->isBusy()) messages << tr("Reading or applying Aero settings…");
    if (!m_session->errorText().isEmpty()) messages << m_session->errorText();
    if (m_session->hasConflict()) messages << tr("Your selections are preserved. Revert to reload saved settings before another Apply.");
    if (m_session->hasPartialFailure()) messages << tr("Some settings may already be saved; Apply was not fully verified.");
    if (!m_session->resultText().isEmpty()) messages << m_session->resultText();
    m_message->setText(messages.join(QLatin1Char('\n'))); m_message->setVisible(!messages.isEmpty());
    m_message->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(m_session->hasConflict() || !m_session->errorText().isEmpty() ? QStringLiteral("#A02C20") : QStringLiteral("#333333")));
    m_restore->setEnabled(m_session->isReady() && !m_session->isBusy());
    m_apply->setEnabled(m_session->isDirty() && m_session->canApply() && !m_session->isBusy() && !m_session->hasConflict());
    m_revert->setEnabled(!m_session->isBusy() && (m_session->isDirty() || m_session->hasConflict() || !m_session->isReady() || !m_session->errorText().isEmpty()));
}
