#include "ActionCenterPage.h"
#include "ConfFile.h"
#include "FirewallStatus.h"
#include "IconHelper.h"
#include "Win7Ui.h"
#include "Branding.h"

#include <QScrollArea>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QFont>
#include <QFile>
#include <QStandardPaths>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>

using Win7::ClickableWidget;

// Data gathering
ActionCenterPage::AcInfo ActionCenterPage::gatherInfo()
{
    AcInfo ac;

    const UfwConfiguredState firewall = readUfwConfiguredState();
    if (!firewall.enabledKnown) {
        ac.firewallState = QStringLiteral("Unknown");
        ac.firewallDescription = QStringLiteral(
            "UFW configuration could not be read or did not contain a recognized enabled setting.");
    } else if (firewall.enabled) {
        ac.firewallState = QStringLiteral("Configured on");
        ac.firewallDescription = QStringLiteral(
            "UFW is configured as enabled. Active firewall enforcement was not checked.");
    } else {
        ac.firewallState = QStringLiteral("Configured off");
        ac.firewallDescription = QStringLiteral(
            "UFW is configured as disabled. Active firewall enforcement was not checked.");
    }

    // A binary probe establishes installation only, not daemon state or coverage.
    if (!QStandardPaths::findExecutable(QStringLiteral("clamscan")).isEmpty()) {
        ac.avPresent = true;
        ac.avName = QStringLiteral("ClamAV");
    }

    // Service registration establishes availability only, not policy coverage.
    if (auto *iface = QDBusConnection::systemBus().interface()) {
        const QDBusReply<bool> registered = iface->isServiceRegistered(
            QStringLiteral("org.freedesktop.PolicyKit1"));
        if (registered.isValid()) {
            ac.polkitKnown = true;
            ac.polkitAvailable = registered.value();
        }
    }

    return ac;
}

// Sidebar
QList<SidebarLink> ActionCenterPage::sidebarLinks()
{
    return {
        Nav::plain("Change Action Center settings"),
        Nav::plain("Change User Account Control settings"),
        Nav::plain("View archived messages"),
        Nav::to("View performance information", PageId::Performance),
    };
}

QList<SidebarLink> ActionCenterPage::sidebarSeeAlso()
{
    return {
        Nav::plain("Backup and Restore"),
        Nav::to("Linux Update", PageId::LinuxUpdate),
        Nav::to("Performance Information and Tools", PageId::Performance),
    };
}

using Win7::bodyLabel;

// Status row
QWidget *ActionCenterPage::buildStatusRow(const QString &item,
                                          const QString &state,
                                          const QString &description,
                                          const QString &link)
{
    auto *row = new QWidget;
    row->setStyleSheet("background: transparent;");
    auto *v = new QVBoxLayout(row);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    // Top line: bold item name on the left, state right-aligned.
    auto *topH = new QHBoxLayout;
    topH->setContentsMargins(0, 0, 0, 0);
    topH->setSpacing(8);

    auto *name = new QLabel(item);
    {
        QFont f = name->font();
        f.setPointSize(9);
        name->setFont(f);
    }
    name->setStyleSheet("color: #000000; background: transparent;");
    topH->addWidget(name, 0, Qt::AlignVCenter);
    topH->addStretch(1);

    auto *stateLabel = new QLabel(state);
    {
        QFont f = stateLabel->font();
        f.setPointSize(9);
        stateLabel->setFont(f);
    }
    stateLabel->setStyleSheet("color: #000000; background: transparent;");
    topH->addWidget(stateLabel, 0, Qt::AlignRight | Qt::AlignVCenter);
    v->addLayout(topH);

    // Description sits indented under the item name, as in the reference.
    auto *desc = bodyLabel(description);
    desc->setContentsMargins(18, 0, 0, 0);
    v->addWidget(desc);

    if (!link.isEmpty()) {
        auto *l = bodyLabel(link, /*link=*/true);
        l->setContentsMargins(18, 0, 0, 0);
        v->addWidget(l);
    }

    return row;
}

// Collapsible section
QWidget *ActionCenterPage::buildSection(const QString &title, bool expanded,
                                        QWidget *rows)
{
    auto *panel = new QWidget;
    panel->setStyleSheet("background: transparent;");
    auto *panelV = new QVBoxLayout(panel);
    panelV->setContentsMargins(0, 0, 0, 0);
    panelV->setSpacing(0);

    // Header strip: blue section title on the left, a round-ish chevron on the
    // right. The whole strip is clickable and toggles the rows below it.
    auto *header = new ClickableWidget;
    header->setAccessibleName(title);
    header->setChecked(expanded);
    header->setStyleSheet("background: transparent;");
    header->setCursor(Qt::PointingHandCursor);
    header->setMinimumHeight(36);
    auto *headerH = new QHBoxLayout(header);
    headerH->setContentsMargins(0, 4, 8, 4);
    headerH->setSpacing(0);

    headerH->addWidget(Win7::label(title, 13, "#1A5FB4"), 0, Qt::AlignVCenter);
    headerH->addStretch(1);

    // The shared round Aero expander. The whole header strip is the click
    // target, so the button itself lets clicks fall through to it.
    auto *chevron = new Win7::ChevronButton(/*interactive=*/false);
    chevron->setChecked(expanded);
    headerH->addWidget(chevron, 0, Qt::AlignVCenter);
    panelV->addWidget(header);

    // Thin separator directly under the header, always visible.
    panelV->addWidget(Win7::hairline("#DCDCDC"));

    // Body carries a left inset so the rows line up under the title text.
    rows->setContentsMargins(2, 12, 2, 12);
    panelV->addWidget(rows);

    // Clicking the header expands/collapses the rows and flips the chevron,
    // matching the Windows Action Center sections.
    auto applyState = [rows, chevron](bool open) {
        rows->setVisible(open);
        chevron->setChecked(open);
    };
    applyState(expanded);
    header->onClick = [applyState, open = expanded]() mutable {
        open = !open;
        applyState(open);
    };

    return panel;
}

// Bottom troubleshooting / recovery task
QWidget *ActionCenterPage::buildBottomTask(
    std::initializer_list<const char *> iconNames,
    const QString &title, const QString &description)
{
    auto *w = new QWidget;
    w->setStyleSheet("background: transparent;");
    w->setEnabled(false);
    w->setToolTip(QStringLiteral("This tool is not implemented in Control Panel yet."));
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(10);

    auto *icon = new QLabel;
    icon->setFixedSize(40, 40);
    icon->setPixmap(themeIcon(iconNames).pixmap(38, 38, QIcon::Disabled));
    icon->setStyleSheet("background: transparent;");
    h->addWidget(icon, 0, Qt::AlignTop);

    auto *col = new QVBoxLayout;
    col->setContentsMargins(0, 2, 0, 0);
    col->setSpacing(2);
    auto *titleLabel = bodyLabel(title);
    auto *descriptionLabel = bodyLabel(description);
    titleLabel->setStyleSheet("color: #777777; background: transparent;");
    descriptionLabel->setStyleSheet("color: #777777; background: transparent;");
    col->addWidget(titleLabel);
    col->addWidget(descriptionLabel);
    h->addLayout(col, 1);

    return w;
}

// Page
ActionCenterPage::ActionCenterPage(QScrollArea *sidebar, QWidget *parent)
    : QWidget(parent)
{
    const AcInfo info = gatherInfo();

    // Windows 7 lays the content out at a fixed width and leaves the rest of
    // the window blank on the right rather than stretching to fill it.
    auto *contentV = Win7::pageScaffold(this, sidebar, /*bottomMargin=*/20,
                                        /*fixedWidth=*/700);

    // Page heading
    contentV->addWidget(
        Win7::pageTitle("Review recent messages and resolve problems", 13));
    contentV->addSpacing(8);

    auto *blurb = bodyLabel(
        "This page shows a limited set of status checks. Items marked “Not monitored” are not checked by this application.");
    contentV->addWidget(blurb);
    contentV->addSpacing(14);

    // ---- Security section -------------------------------------------------
    auto *securityRows = new QWidget;
    securityRows->setStyleSheet("background: transparent;");
    {
        auto *v = new QVBoxLayout(securityRows);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(14);
        v->addWidget(buildStatusRow(
            "UFW firewall configuration", info.firewallState,
            info.firewallDescription));
        v->addWidget(buildStatusRow(
            Branding::brand("Linux Update"), "Not monitored",
            "This page does not check whether updates are available or install automatically."));
        v->addWidget(buildStatusRow(
            "Spyware and unwanted software protection",
            info.avPresent ? "Scanner installed" : "No scanner found",
            info.avPresent
                ? QStringLiteral("%1 is installed; active protection and scan status were not checked.")
                      .arg(info.avName)
                : QStringLiteral("No supported scanner executable was found; protection status is unknown.")));
        v->addWidget(buildStatusRow(
            "Internet security settings", "Not monitored",
            "This page does not inspect browser, DNS, or network security settings."));
        v->addWidget(buildStatusRow(
            "Authentication service", !info.polkitKnown ? "Unknown"
                : info.polkitAvailable ? "Available" : "Not detected",
            !info.polkitKnown ? "Authentication service status could not be read."
                : info.polkitAvailable
                ? "A polkit authority is registered. This does not verify that every application uses it."
                : "No polkit authority was detected on the system bus."));
        v->addWidget(buildStatusRow(
            "Network Access Protection", "Not monitored",
            "This Windows service has no status check in this application."));
    }
    contentV->addWidget(buildSection("Security", /*expanded=*/false,
                                     securityRows));

    // ---- Maintenance section ---------------------------------------------
    auto *maintRows = new QWidget;
    maintRows->setStyleSheet("background: transparent;");
    {
        auto *v = new QVBoxLayout(maintRows);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(14);
        v->addWidget(buildStatusRow(
            "Problem reporting", "Not monitored",
            "No problem-reporting service status is checked."));
        v->addWidget(buildStatusRow(
            "Backup", "Unknown",
            "Backup configuration is not checked by this application."));
        v->addWidget(buildStatusRow(
            "Check for updates", "Not monitored",
            "Open Linux Update to check for package updates."));
        v->addWidget(buildStatusRow(
            "System maintenance", "Not monitored",
            "No maintenance checks run in the background."));
    }
    contentV->addWidget(buildSection("Maintenance", /*expanded=*/false,
                                     maintRows));
    contentV->addSpacing(14);

    contentV->addSpacing(24);

    // ---- Bottom "If you don't see your problem listed" --------------------
    auto *bottomIntro = bodyLabel(
        "These additional tools are not implemented in Control Panel yet:");
    contentV->addWidget(bottomIntro);
    contentV->addSpacing(12);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(40);
    bottomRow->addWidget(buildBottomTask(
        {"tools-report-bug", "system-run", "preferences-system"},
        "Troubleshooting", "This application does not provide system diagnostics."),
        0, Qt::AlignTop);
    bottomRow->addWidget(buildBottomTask(
        {"document-revert", "chronometer", "system-reboot"},
        "Recovery", "System restore points are not managed by this application."), 0,
        Qt::AlignTop);
    bottomRow->addStretch(1);
    contentV->addLayout(bottomRow);

    contentV->addStretch(1);
}
