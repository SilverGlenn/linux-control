#include "PackagePreviewPage.h"
#include "Win7Ui.h"
#include "packages/PackageSimulationService.h"
#include <QHeaderView>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>

PackagePreviewPage::PackagePreviewPage(QScrollArea *sidebar, QWidget *parent)
    : PackagePreviewPage(sidebar, PackageSimulationService::instance(), parent) {}
PackagePreviewPage::PackagePreviewPage(QScrollArea *sidebar, PackageSimulationService *service, QWidget *parent)
    : QWidget(parent), m_service(service)
{
    Q_ASSERT(service);
    auto *layout = Win7::pageScaffold(this, sidebar, 20);
    Win7::styleSettingsControls(this);
    layout->addWidget(Win7::pageTitle(tr("Preview package changes")));
    layout->addSpacing(10);
    auto *intro = Win7::label(tr("See which packages APT would change in a full upgrade using your current installed packages and cached package indexes."));
    intro->setWordWrap(true); layout->addWidget(intro);
    auto *scope = Win7::label(tr("This preview performs no installation, removal, download, or package-index refresh. Cached indexes may be out of date. APT does not lock package state during simulation, so changes elsewhere can make the result stale."), 9, "#555555");
    scope->setWordWrap(true); layout->addWidget(scope); layout->addSpacing(12);
    auto *buttons = new QHBoxLayout;
    m_start = new QPushButton(tr("&Preview full upgrade")); m_start->setObjectName(QStringLiteral("startPackagePreview"));
    m_cancel = new QPushButton(tr("&Cancel preview")); m_cancel->setObjectName(QStringLiteral("cancelPackagePreview"));
    buttons->addWidget(m_start); buttons->addWidget(m_cancel); buttons->addStretch(); layout->addLayout(buttons);
    m_status = Win7::label(QString()); m_status->setObjectName(QStringLiteral("packagePreviewStatus"));
    m_message = Win7::label(QString(), 9, "#A02C20"); m_message->setObjectName(QStringLiteral("packagePreviewMessage"));
    for (auto *label : {m_status, m_message}) { label->setWordWrap(true); label->setTextFormat(Qt::PlainText); layout->addWidget(label); }
    m_changes = new QTreeWidget;
    m_changes->setObjectName(QStringLiteral("packagePreviewChanges")); m_changes->setAccessibleName(tr("Simulated package changes"));
    m_changes->setHeaderLabels({tr("Change"), tr("Package"), tr("Installed version"), tr("Proposed version"), tr("Package source")});
    Win7::configureListTree(m_changes); m_changes->setMinimumHeight(230); m_changes->setSortingEnabled(true);
    m_changes->setStyleSheet(QStringLiteral(
        "QTreeWidget { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; }"
        "QTreeWidget::item:selected { color: #000000; background: #CDE6F7; }"));
    m_changes->header()->setStyleSheet(m_changes->header()->styleSheet() + QStringLiteral(" QHeaderView::section { color: #333333; }"));
    m_changes->header()->setSectionResizeMode(QHeaderView::ResizeToContents); m_changes->header()->setStretchLastSection(true);
    layout->addWidget(m_changes);
    layout->addLayout(Win7::sectionHeading(tr("APT output and diagnostics")));
    m_details = new QPlainTextEdit; m_details->setReadOnly(true); m_details->setObjectName(QStringLiteral("packagePreviewDetails"));
    m_details->setAccessibleName(tr("APT simulation output and diagnostics")); m_details->setMaximumBlockCount(30000); m_details->setMinimumHeight(130);
    m_details->setStyleSheet(QStringLiteral("QPlainTextEdit { color: #222222; background: #FFFFFF; border: 1px solid #ABADB3; }"));
    layout->addWidget(m_details);
    auto *limits = Win7::label(tr("This page uses APT's built-in resolver with external hooks disabled. Custom package tools can choose different changes. No package changes can be applied from this page; other package managers and distribution-release upgrades are outside this preview."), 9, "#555555");
    limits->setWordWrap(true); layout->addWidget(limits); layout->addStretch();
    connect(m_start, &QPushButton::clicked, service, &PackageSimulationService::startPreview);
    connect(m_cancel, &QPushButton::clicked, service, &PackageSimulationService::cancel);
    connect(service, &PackageSimulationService::changed, this, &PackagePreviewPage::updateState);
    updateState();
}
QList<SidebarLink> PackagePreviewPage::sidebarLinks()
{ return {Nav::to("Programs and Features", PageId::ProgramsFeatures), Nav::to("Linux Update", PageId::LinuxUpdate)}; }
QList<SidebarLink> PackagePreviewPage::sidebarSeeAlso()
{ return {Nav::to("System", PageId::System)}; }
void PackagePreviewPage::updateState()
{
    const auto result = m_service->result();
    m_status->setText(m_service->statusText() + (result.completedAt.isValid() ? tr("\nPreview finished: %1").arg(QLocale().toString(result.completedAt, QLocale::ShortFormat)) : QString()));
    QString message = result.error;
    if (!m_service->providerAvailable()) message = m_service->unavailableReason();
    if (!result.warnings.isEmpty()) message += (message.isEmpty() ? QString() : QStringLiteral("\n")) + tr("APT diagnostics:\n%1").arg(result.warnings);
    m_message->setText(message); m_message->setVisible(!message.isEmpty());
    m_start->setEnabled(!m_service->isBusy() && m_service->providerAvailable());
    m_cancel->setEnabled(m_service->state() == PackageSimulationService::State::Running);
    m_changes->clear();
    if (result.success) {
        m_changes->setSortingEnabled(false);
        for (const auto &change : result.changes) {
            QString action;
            switch (change.action) {
            case PackageChange::Action::Upgrade: action = tr("Upgrade"); break;
            case PackageChange::Action::Install: action = tr("Install dependency"); break;
            case PackageChange::Action::Remove: action = tr("Remove"); break;
            case PackageChange::Action::Configure: action = tr("Configure"); break;
            }
            new QTreeWidgetItem(m_changes, {action, change.package, change.installedVersion, change.candidateVersion, change.source});
        }
        m_changes->setSortingEnabled(true);
    }
    m_details->setPlainText(result.rawOutput + (result.warnings.isEmpty() ? QString() : QStringLiteral("\nDiagnostics:\n") + result.warnings));
}
