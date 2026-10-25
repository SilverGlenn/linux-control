#include "FontSettingsPage.h"
#include "Win7Ui.h"
#include "settings/FontSettings.h"
#include <QComboBox>
#include <QFontDialog>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>

FontSettingsPage::FontSettingsPage(QScrollArea *sidebar, QWidget *parent)
    : FontSettingsPage(sidebar, FontSettings::instance(), parent) {}
FontSettingsPage::FontSettingsPage(QScrollArea *sidebar, FontSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 760);
    Win7::styleSettingsControls(this);
    layout->addWidget(Win7::pageTitle(tr("Change font settings")));
    layout->addSpacing(10);
    auto *intro = Win7::label(tr("Choose fonts for the KDE desktop and applications. Your choices are staged until you click Apply."));
    intro->setWordWrap(true); layout->addWidget(intro); layout->addSpacing(18);
    layout->addLayout(Win7::sectionHeading(tr("Desktop fonts")));
    const QStringList roles{tr("&General:"), tr("&Fixed width:"), tr("&Small text:"), tr("&Toolbar:"), tr("&Menu:"), tr("&Window title:")};
    const QStringList names{QStringLiteral("generalFont"), QStringLiteral("fixedFont"), QStringLiteral("smallFont"), QStringLiteral("toolbarFont"), QStringLiteral("menuFont"), QStringLiteral("windowTitleFont")};
    for (int i = 0; i < 6; ++i) {
        auto *row = new QHBoxLayout;
        auto *label = Win7::label(roles[i]); label->setMinimumWidth(125);
        auto *button = new QPushButton;
        button->setObjectName(names[i]); button->setAccessibleName(QString(roles[i]).remove(QLatin1Char('&')).remove(QLatin1Char(':')) + tr(" font"));
        button->setMinimumWidth(310); label->setBuddy(button); m_fonts[i] = button;
        row->addWidget(label); row->addWidget(button); row->addStretch(); layout->addLayout(row);
        connect(button, &QPushButton::clicked, this, [this, i] {
            auto *dialog = new QFontDialog(m_session->stagedPreferences().fonts[i], this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(tr("Choose desktop font"));
            dialog->setOption(QFontDialog::DontUseNativeDialog);
            Win7::styleSettingsDialog(dialog);
            if (i == 1) dialog->setOption(QFontDialog::MonospacedFonts);
            connect(dialog, &QFontDialog::fontSelected, this, [this, i](const QFont &font) { m_session->selectFont(i, font); });
            dialog->open();
        });
    }
    layout->addSpacing(20); layout->addLayout(Win7::sectionHeading(tr("Font rendering")));
    auto comboRow = [this, layout](const QString &labelText, const QString &name) {
        auto *row = new QHBoxLayout; auto *label = Win7::label(labelText); label->setMinimumWidth(125);
        auto *combo = new QComboBox; combo->setMinimumWidth(310); combo->setObjectName(name);
        combo->setAccessibleName(QString(labelText).remove(QLatin1Char('&')).remove(QLatin1Char(':')));
        label->setBuddy(combo); row->addWidget(label); row->addWidget(combo); row->addStretch(); layout->addLayout(row);
        combo->addItem(tr("Vendor default (no local override)"), QString()); return combo;
    };
    m_antialias = comboRow(tr("&Antialiasing:"), QStringLiteral("fontAntialiasing"));
    m_antialias->addItem(tr("Enabled"), QStringLiteral("true")); m_antialias->addItem(tr("Disabled"), QStringLiteral("false"));
    m_hinting = comboRow(tr("&Hinting:"), QStringLiteral("fontHinting"));
    m_hinting->addItem(tr("None"), QStringLiteral("hintnone")); m_hinting->addItem(tr("Slight"), QStringLiteral("hintslight"));
    m_hinting->addItem(tr("Medium"), QStringLiteral("hintmedium")); m_hinting->addItem(tr("Full"), QStringLiteral("hintfull"));
    m_subpixel = comboRow(tr("Subpi&xel order:"), QStringLiteral("fontSubpixel"));
    for (const auto &pair : QList<QPair<QString, QString>>{{tr("None"), "none"}, {tr("RGB"), "rgb"}, {tr("BGR"), "bgr"}, {tr("Vertical RGB"), "vrgb"}, {tr("Vertical BGR"), "vbgr"}})
        m_subpixel->addItem(pair.first, pair.second);
    for (auto *combo : {m_antialias, m_hinting, m_subpixel})
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { m_session->selectRendering(m_antialias->currentData().toString(), m_hinting->currentData().toString(), m_subpixel->currentData().toString()); });
    auto *scope = Win7::label(tr("Rendering controls edit local fontconfig overrides. Vendor defaults and application-specific or size-specific rules may still apply. Restart applications after changing rendering; some applications require signing out and back in."), 9, "#555555");
    scope->setWordWrap(true); layout->addWidget(scope); layout->addSpacing(16);
    m_status = Win7::label(QString()); m_status->setObjectName(QStringLiteral("fontSettingsStatus"));
    m_message = Win7::label(QString(), 9, "#A02C20"); m_message->setObjectName(QStringLiteral("fontSettingsMessage"));
    for (auto *label : {m_status, m_message}) { label->setWordWrap(true); label->setTextFormat(Qt::PlainText); layout->addWidget(label); }
    auto *buttons = new QHBoxLayout; buttons->addStretch();
    m_apply = new QPushButton(tr("&Apply")); m_apply->setObjectName(QStringLiteral("applyFontSettings"));
    m_revert = new QPushButton(tr("&Revert")); m_revert->setObjectName(QStringLiteral("revertFontSettings"));
    buttons->addWidget(m_apply); buttons->addWidget(m_revert); layout->addLayout(buttons); layout->addStretch();
    connect(m_apply, &QPushButton::clicked, session, &FontSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &FontSettings::revert);
    connect(session, &SettingsSession::stateChanged, this, &FontSettingsPage::updateState);
    updateState();
}
SettingsSession *FontSettingsPage::settingsSession() const { return m_session; }
QList<SidebarLink> FontSettingsPage::sidebarLinks()
{ return {Nav::to("Fonts", PageId::Fonts), Nav::to("Personalization", PageId::Personalization)}; }
QList<SidebarLink> FontSettingsPage::sidebarSeeAlso()
{ return {Nav::to("Ease of Access Center", PageId::EaseOfAccess)}; }
void FontSettingsPage::updateState()
{
    const auto p = m_session->stagedPreferences();
    const bool editable = m_session->isReady() && m_session->canApply() && !m_session->isBusy();
    for (int i = 0; i < 6; ++i) {
        m_fonts[i]->setText(m_session->isReady() ? tr("%1, %2 pt — %3").arg(p.fonts[i].family()).arg(p.fonts[i].pointSizeF())
            .arg(p.fonts[i].styleName().isEmpty() ? (p.fonts[i].bold() && p.fonts[i].italic() ? tr("Bold Italic")
                : p.fonts[i].italic() ? tr("Italic") : p.fonts[i].bold() ? tr("Bold") : tr("Regular")) : p.fonts[i].styleName()) : tr("Unavailable"));
        m_fonts[i]->setEnabled(editable);
    }
    auto update = [editable](QComboBox *combo, const QString &value) {
        const QSignalBlocker blocker(combo); combo->setCurrentIndex(combo->findData(value)); combo->setEnabled(editable);
    };
    update(m_antialias, p.antialias); update(m_hinting, p.hinting); update(m_subpixel, p.subpixel);
    m_status->setText(m_session->isBusy() ? (m_session->pendingMessage().isEmpty() ? tr("Working…") : m_session->pendingMessage()) : !m_session->isReady() ? tr("Saved font settings are unavailable.")
        : m_session->isDirty() ? tr("Your selected fonts and rendering settings have not been applied.") : tr("Showing saved font settings."));
    QString message = m_session->errorText();
    if (m_session->hasConflict()) message += tr("\nYour selection is preserved. Revert reloads saved settings before another Apply.");
    if (!m_session->canApply() && !m_session->unavailableReason().isEmpty()) message += QLatin1Char('\n') + m_session->unavailableReason();
    if (message.trimmed().isEmpty()) message = m_session->resultText();
    m_message->setText(message.trimmed()); m_message->setVisible(!message.trimmed().isEmpty());
    m_message->setStyleSheet(m_session->hasConflict() || !m_session->errorText().isEmpty()
        ? QStringLiteral("color: #A02C20; background: transparent;")
        : QStringLiteral("color: #333333; background: transparent;"));
    m_apply->setEnabled(editable && m_session->isDirty() && !m_session->hasConflict());
    m_revert->setEnabled(!m_session->isBusy() && (m_session->isDirty() || m_session->hasConflict() || !m_session->isReady() || !m_session->errorText().isEmpty()));
}
