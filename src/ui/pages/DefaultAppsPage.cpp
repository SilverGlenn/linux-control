#include "DefaultAppsPage.h"
#include "Win7Ui.h"
#include <QComboBox>
#include <QCoreApplication>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMimeDatabase>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>

DefaultAppsPage::DefaultAppsPage(QScrollArea *sidebar, QWidget *parent)
    : DefaultAppsPage(sidebar, DefaultAppsSettings::instance(), parent) {}
DefaultAppsPage::DefaultAppsPage(QScrollArea *sidebar, DefaultAppsSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    setObjectName(QStringLiteral("defaultAppsPage"));
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 900);
    Win7::styleSettingsControls(this);
    layout->addWidget(Win7::pageTitle(tr("Choose your default programs")));
    auto *intro = Win7::label(tr("Choose which programs open your files and handle common tasks. Changes are saved when you click Apply."));
    intro->setWordWrap(true); layout->addWidget(intro); layout->addSpacing(14);
    layout->addLayout(Win7::sectionHeading(tr("Set your default programs")));
    for (const auto &r : DefaultAppsTypes::roles()) {
        auto *row = new QHBoxLayout;
        auto *label = Win7::label(QCoreApplication::translate("DefaultAppsPage", r.label.toUtf8().constData()) + QLatin1Char(':'));
        label->setMinimumWidth(155);
        auto *combo = new QComboBox; combo->setMinimumWidth(320);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); combo->setMinimumContentsLength(24);
        combo->setObjectName(QStringLiteral("defaultRole_") + r.key); combo->setAccessibleName(label->text()); label->setBuddy(combo);
        combo->setToolTip(r.role == DefaultAppRole::Terminal ? tr("Uses the selected installed desktop entry's command and identity.")
            : tr("Sets the common writable types supported by the selected application: %1").arg(r.mimeTypes.join(QStringLiteral(", "))));
        m_roles[r.role] = combo; row->addWidget(label); row->addWidget(combo); row->addStretch(); layout->addLayout(row);
        connect(combo, &QComboBox::activated, this, [this, role = r.role, combo] {
            if (!m_updating) m_session->selectRole(role, combo->currentData().toString());
        });
    }
    auto *scope = Win7::label(tr("Each program is offered for roles and file types it declares it can handle. A role changes only its common supported types. Use the list below to choose a program for an individual file type."), 9, "#555555");
    scope->setWordWrap(true); layout->addWidget(scope); layout->addSpacing(18);
    layout->addLayout(Win7::sectionHeading(tr("Associate a file type or protocol with a program")));
    m_search = new QLineEdit; m_search->setObjectName(QStringLiteral("defaultAppsSearch"));
    m_search->setPlaceholderText(tr("Search file types, extensions, or protocols")); m_search->setAccessibleName(tr("Search file associations"));
    layout->addWidget(m_search);
    m_types = new QTreeWidget; m_types->setObjectName(QStringLiteral("defaultAppsMimeTypes"));
    m_types->setHeaderLabels({tr("Name"), tr("Description"), tr("Saved default program")}); m_types->setRootIsDecorated(false);
    m_types->setSelectionMode(QAbstractItemView::SingleSelection); m_types->setUniformRowHeights(true); m_types->setMinimumHeight(220);
    m_types->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_types->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_types->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(m_types);
    m_mimeDescription = Win7::label(QString()); m_mimeDescription->setWordWrap(true); m_mimeDescription->setTextFormat(Qt::PlainText);
    layout->addWidget(m_mimeDescription);
    auto *mimeRow = new QHBoxLayout; auto *openWith = Win7::label(tr("Open &with:"));
    m_mimeApps = new QComboBox; m_mimeApps->setObjectName(QStringLiteral("defaultAppsMimeApplication"));
    m_mimeApps->setMinimumWidth(310); m_mimeApps->setAccessibleName(tr("Program for the selected file type")); openWith->setBuddy(m_mimeApps);
    m_change = new QPushButton(tr("&Change program")); m_change->setObjectName(QStringLiteral("changeDefaultMimeProgram"));
    mimeRow->addWidget(openWith); mimeRow->addWidget(m_mimeApps); mimeRow->addWidget(m_change); mimeRow->addStretch(); layout->addLayout(mimeRow);
    connect(m_search, &QLineEdit::textChanged, this, &DefaultAppsPage::updateAssociations);
    connect(m_types, &QTreeWidget::currentItemChanged, this, [this] { if (!m_updating) updateMimeChoices(); });
    connect(m_mimeApps, &QComboBox::currentIndexChanged, this, [this] {
        m_change->setEnabled(!m_session->isBusy() && m_session->canApply() && m_mimeApps->currentIndex() >= 0);
    });
    connect(m_change, &QPushButton::clicked, this, [this] {
        if (auto *item = m_types->currentItem()) m_session->selectMime(item->data(0, Qt::UserRole).toString(), m_mimeApps->currentData().toString());
    });
    layout->addSpacing(14);
    m_status = Win7::label(QString()); m_status->setObjectName(QStringLiteral("defaultAppsStatus")); m_status->setWordWrap(true);
    m_message = Win7::label(QString(), 9, "#A02C20"); m_message->setObjectName(QStringLiteral("defaultAppsMessage"));
    m_message->setWordWrap(true); m_message->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status); layout->addWidget(m_message);
    auto *buttons = new QHBoxLayout;
    m_defaults = new QPushButton(tr("Restore &defaults")); m_defaults->setObjectName(QStringLiteral("restoreDefaultApps"));
    m_defaults->setToolTip(tr("Stages KDE's recommended applications only where they are installed and compatible."));
    m_apply = new QPushButton(tr("&Apply")); m_apply->setObjectName(QStringLiteral("applyDefaultApps"));
    m_revert = new QPushButton(tr("&Revert")); m_revert->setObjectName(QStringLiteral("revertDefaultApps"));
    buttons->addWidget(m_defaults); buttons->addStretch(); buttons->addWidget(m_apply); buttons->addWidget(m_revert);
    layout->addLayout(buttons); layout->addStretch();
    connect(m_defaults, &QPushButton::clicked, session, &DefaultAppsSettings::restoreDefaults);
    connect(m_apply, &QPushButton::clicked, session, &DefaultAppsSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &DefaultAppsSettings::revert);
    connect(session, &SettingsSession::stateChanged, this, &DefaultAppsPage::updateState);
    updateState();
}
SettingsSession *DefaultAppsPage::settingsSession() const { return m_session; }
QList<SidebarLink> DefaultAppsPage::sidebarLinks()
{ return {Nav::to("Programs and Features", PageId::ProgramsFeatures)}; }
QList<SidebarLink> DefaultAppsPage::sidebarSeeAlso()
{ return {Nav::to("Personalization", PageId::Personalization)}; }
void DefaultAppsPage::updateAssociations()
{
    const QString filter = m_search->text().trimmed();
    const auto &state = m_session->applied(); const auto &staged = m_session->staged();
    const bool rebuild = m_inventoryRevision != state.revision;
    const QString selected = m_types->currentItem() ? m_types->currentItem()->data(0, Qt::UserRole).toString() : QString();
    m_updating = true; QSignalBlocker block(m_types);
    if (rebuild) {
        m_types->clear();
        for (const auto &mime : state.mimeTypes) {
            const auto type = QMimeDatabase().mimeTypeForName(mime);
            const QString description = type.isValid() ? type.comment() : tr("Protocol handler");
            auto *item = new QTreeWidgetItem(m_types, {mime, description, QString()});
            item->setData(0, Qt::UserRole, mime);
            item->setData(0, Qt::UserRole + 1, mime + QLatin1Char(' ') + description + QLatin1Char(' ') + type.globPatterns().join(QLatin1Char(' ')));
            if (mime == selected) m_types->setCurrentItem(item);
        }
        m_inventoryRevision = state.revision;
    }
    for (int i = 0; i < m_types->topLevelItemCount(); ++i) {
        auto *item = m_types->topLevelItem(i); const QString mime = item->data(0, Qt::UserRole).toString();
        const QString id = staged.mimeDefaults.value(mime); const auto *app = state.application(id);
        item->setText(2, app ? app->name : id.isEmpty() ? tr("System selection") : id);
        if (id != state.preferences.mimeDefaults.value(mime)) item->setText(2, item->text(2) + tr(" (pending)"));
        item->setHidden(!filter.isEmpty() && !item->data(0, Qt::UserRole + 1).toString().contains(filter, Qt::CaseInsensitive));
    }
    if (!m_types->currentItem() || m_types->currentItem()->isHidden()) {
        m_types->setCurrentItem(nullptr);
        for (int i = 0; i < m_types->topLevelItemCount(); ++i) if (!m_types->topLevelItem(i)->isHidden()) { m_types->setCurrentItem(m_types->topLevelItem(i)); break; }
    }
    m_updating = false; updateMimeChoices();
}
void DefaultAppsPage::updateMimeChoices()
{
    const QSignalBlocker blocker(m_mimeApps); m_mimeApps->clear();
    const auto *item = m_types->currentItem(); const QString mime = item ? item->data(0, Qt::UserRole).toString() : QString();
    const auto &state = m_session->applied();
    for (const auto &app : state.applications) if (DefaultAppsTypes::supports(app, mime))
        m_mimeApps->addItem(QIcon::fromTheme(app.icon), app.name + QStringLiteral(" (") + app.id + QLatin1Char(')'), app.id);
    const int selected = m_mimeApps->findData(m_session->staged().mimeDefaults.value(mime));
    m_mimeApps->setCurrentIndex(selected >= 0 ? selected : m_mimeApps->count() ? 0 : -1);
    const bool enabled = m_session->canApply() && !m_session->isBusy() && state.canChangeMime(mime) && m_mimeApps->count() > 0;
    m_mimeApps->setEnabled(enabled); m_change->setEnabled(enabled);
    m_mimeDescription->setText(mime.isEmpty() ? tr("Select a file type or protocol.") : m_mimeApps->count() ? tr("%1 — select an installed compatible program and click Change program to stage this association.").arg(mime)
        : tr("%1 — no installed application declares support for this type.").arg(mime));
}
void DefaultAppsPage::updateState()
{
    m_updating = true;
    const auto &state = m_session->applied(); const bool editable = m_session->canApply() && !m_session->isBusy();
    for (const auto &r : DefaultAppsTypes::roles()) {
        auto *combo = m_roles.value(r.role); const QSignalBlocker blocker(combo); combo->clear();
        const QString id = DefaultAppsTypes::selected(m_session->staged(), r);
        const auto *selected = state.application(id);
        if (id.isEmpty()) combo->addItem(tr("System selection (no saved default)"), QString());
        else if (!selected || !DefaultAppsTypes::eligible(*selected, r)) combo->addItem(selected ? selected->name + tr(" (current)") : id + tr(" (unavailable)"), id);
        int compatible = 0;
        for (const auto &app : state.applications) if (DefaultAppsTypes::eligible(app, r)) {
            combo->addItem(QIcon::fromTheme(app.icon), app.name + QStringLiteral(" (") + app.id + QLatin1Char(')'), app.id); ++compatible;
        }
        combo->setCurrentIndex(combo->findData(id)); combo->setEnabled(editable && compatible > 0 && !state.unavailableRoles.contains(r.role));
        if (state.unavailableRoles.contains(r.role)) combo->setToolTip(state.unavailableRoles.value(r.role));
    }
    m_updating = false; updateAssociations();
    m_search->setEnabled(m_session->isReady());
    m_status->setText(m_session->isBusy() ? tr("Working…") : !m_session->isReady() ? tr("Default programs are unavailable.")
        : m_session->isDirty() ? tr("Your selected default programs have not been applied.")
        : tr("Showing validated saved defaults. System selection means no valid saved preference was found; running service-cache choices are not inspected."));
    QString message = m_session->errorText();
    if (m_session->hasConflict()) message += tr("\nYour choices are preserved. Revert reloads saved settings before another Apply.");
    if (m_session->isReady() && !state.writable) message += QLatin1Char('\n') + state.reason;
    if (message.trimmed().isEmpty()) message = m_session->resultText();
    m_message->setText(message.trimmed()); m_message->setVisible(!message.trimmed().isEmpty());
    m_message->setStyleSheet(m_session->hasConflict() || !m_session->errorText().isEmpty() ? QStringLiteral("color: #A02C20; background: transparent;") : QStringLiteral("color: #333333; background: transparent;"));
    m_apply->setEnabled(editable && m_session->isDirty()); m_defaults->setEnabled(editable);
    m_revert->setEnabled(!m_session->isBusy() && (m_session->isDirty() || m_session->hasConflict() || !m_session->isReady() || !m_session->errorText().isEmpty()));
}
