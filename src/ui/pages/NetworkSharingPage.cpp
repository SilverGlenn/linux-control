#include "NetworkSharingPage.h"
#include "settings/NetworkSettings.h"
#include "Win7Ui.h"
#include "LinkLabel.h"
#include <QHostInfo>
#include <QLabel>
#include <QScrollArea>
#include <QPushButton>

namespace {
QString connectivityText(uint state)
{
    switch (state) {
    case 1: return QStringLiteral("No connectivity");
    case 2: return QStringLiteral("Sign-in required (captive portal)");
    case 3: return QStringLiteral("Limited connectivity");
    case 4: return QStringLiteral("Full connectivity reported by NetworkManager");
    default: return QStringLiteral("Internet connectivity unknown");
    }
}
QWidget *mapNode(const QString &icon, QLabel **caption)
{
    auto *node = new QWidget;
    auto *layout = new QVBoxLayout(node);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *image = new QLabel;
    image->setPixmap(themeIcon({icon.toUtf8().constData(), "preferences-system-network"}).pixmap(40, 40));
    layout->addWidget(image, 0, Qt::AlignHCenter);
    *caption = Win7::bodyLabel(QString());
    (*caption)->setTextFormat(Qt::PlainText);
    (*caption)->setAlignment(Qt::AlignHCenter);
    layout->addWidget(*caption);
    node->setMinimumWidth(135);
    return node;
}
}

NetworkSharingPage::NetworkSharingPage(QScrollArea *sidebar, QWidget *parent)
    : NetworkSharingPage(sidebar, NetworkSettings::instance(), parent) {}
NetworkSharingPage::NetworkSharingPage(QScrollArea *sidebar, NetworkSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 760);
    Win7::styleSettingsControls(this);
    layout->addWidget(Win7::pageTitle(QStringLiteral("View your basic network information and set up connections")));
    layout->addSpacing(24);
    auto *map = new QHBoxLayout;
    QLabel *host = nullptr;
    map->addWidget(mapNode(QStringLiteral("computer"), &host), 1);
    host->setText(QHostInfo::localHostName() + QStringLiteral("\n(This computer)"));
    map->addWidget(Win7::label(QStringLiteral("—"), 18, "#6E94C0"));
    map->addWidget(mapNode(QStringLiteral("network-workgroup"), &m_network), 1);
    map->addWidget(Win7::label(QStringLiteral("—"), 18, "#6E94C0"));
    map->addWidget(mapNode(QStringLiteral("network-globe"), &m_connectivity), 1);
    m_network->setObjectName(QStringLiteral("networkMapNetwork"));
    m_connectivity->setObjectName(QStringLiteral("networkConnectivity"));
    layout->addLayout(map);
    layout->addSpacing(22);
    layout->addLayout(Win7::sectionHeading(QStringLiteral("View your active networks")));
    layout->addSpacing(10);
    m_active = Win7::bodyLabel(QString());
    m_active->setObjectName(QStringLiteral("networkActiveConnections"));
    m_active->setTextFormat(Qt::PlainText);
    layout->addWidget(m_active);
    layout->addSpacing(15);
    m_status = Win7::bodyLabel(QString());
    m_status->setObjectName(QStringLiteral("networkSharingStatus"));
    m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status);
    m_confirmation = Win7::bodyLabel(QString());
    m_confirmation->setObjectName(QStringLiteral("networkSharingConfirmation"));
    m_confirmation->setTextFormat(Qt::PlainText);
    layout->addWidget(m_confirmation);
    auto *confirmationButtons = new QHBoxLayout;
    m_keep = new QPushButton(QStringLiteral("&Keep changes"));
    m_keep->setObjectName(QStringLiteral("networkSharingKeep"));
    m_keep->setAccessibleName(QStringLiteral("Keep network changes"));
    m_undo = new QPushButton(QStringLiteral("Re&vert now"));
    m_undo->setObjectName(QStringLiteral("networkSharingRevertPreview"));
    m_undo->setAccessibleName(QStringLiteral("Revert network changes now"));
    confirmationButtons->addWidget(m_keep);
    confirmationButtons->addWidget(m_undo);
    confirmationButtons->addStretch();
    layout->addLayout(confirmationButtons);
    connect(m_keep, &QPushButton::clicked, session, &NetworkSettings::keep);
    connect(m_undo, &QPushButton::clicked, session, &NetworkSettings::revert);
    layout->addSpacing(20);
    layout->addLayout(Win7::sectionHeading(QStringLiteral("Change your networking settings")));
    auto addTask = [this, layout](const QString &title, const QString &description, const QString &name) {
        layout->addSpacing(14);
        auto *link = new LinkLabel(title);
        link->setObjectName(name);
        layout->addWidget(link);
        connect(link, &LinkLabel::clicked, this, [this] { emit navigationRequested(PageId::NetworkConnections); });
        layout->addSpacing(3);
        layout->addWidget(Win7::bodyLabel(description));
    };
    addTask(QStringLiteral("Connect to a network"),
        QStringLiteral("Choose an Ethernet or Wi-Fi adapter, connect to a saved connection, or scan for nearby wireless networks."),
        QStringLiteral("networkConnectLink"));
    addTask(QStringLiteral("Change adapter settings"),
        QStringLiteral("Manage connection properties, automatic addressing, manual IPv4 and IPv6 addresses, and DNS servers."),
        QStringLiteral("networkAdapterLink"));
    layout->addSpacing(18);
    layout->addWidget(Win7::bodyLabel(QStringLiteral("File and printer sharing, Windows HomeGroup, enterprise Wi-Fi and WEP configuration are not available here.")));
    layout->addStretch();
    connect(session, &SettingsSession::stateChanged, this, &NetworkSharingPage::updateState);
    updateState();
}
SettingsSession *NetworkSharingPage::settingsSession() const { return m_session; }
QList<SidebarLink> NetworkSharingPage::sidebarLinks()
{
    return {Nav::to(QStringLiteral("Change adapter settings"), PageId::NetworkConnections)};
}
QList<SidebarLink> NetworkSharingPage::sidebarSeeAlso()
{
    return {Nav::to(QStringLiteral("Linux Firewall"), PageId::Firewall)};
}
void NetworkSharingPage::updateState()
{
    const auto &snapshot = m_session->snapshot();
    const bool available = m_session->isReady() && snapshot.serviceAvailable;
    m_connectivity->setText(available ? connectivityText(snapshot.connectivity)
                                     : QStringLiteral("Connectivity unknown"));
    QStringList active;
    for (const auto &device : snapshot.devices) {
        // Loopback and Wi-Fi Direct discovery devices are adapter inventory,
        // rather than user network connections in this overview.
        if (device.type == 32 || device.type == 30) continue;
        if (device.activeConnectionPath.isEmpty() || device.activeConnectionPath == QStringLiteral("/")) continue;
        const auto *profile = snapshot.profile(device.activeProfilePath);
        active << QStringLiteral("%1 — %2\n%3%4").arg(profile ? profile->id : QStringLiteral("Active connection"),
            device.interfaceName, device.ipv4Addresses.join(QStringLiteral(", ")),
            device.ipv6Addresses.isEmpty() ? QString() : QStringLiteral("\n") + device.ipv6Addresses.join(QStringLiteral(", ")));
    }
    m_network->setText(!available ? QStringLiteral("Network status unavailable")
        : active.isEmpty() ? QStringLiteral("No active network") : QStringLiteral("%1 active connection(s)").arg(active.size()));
    m_active->setText(!available && m_session->isBusy() ? QStringLiteral("Reading network information…")
        : !available ? QStringLiteral("NetworkManager is unavailable.")
        : active.isEmpty() ? QStringLiteral("You are not currently connected to a network.") : active.join(QStringLiteral("\n\n")));
    QString status = !m_session->errorText().isEmpty() ? m_session->errorText() : m_session->resultText();
    if (status.isEmpty()) status = snapshot.serviceAvailable
        ? QStringLiteral("NetworkManager %1. Internet status is reported by the service; a default route alone does not confirm Internet access.").arg(snapshot.version)
        : snapshot.reason;
    if (snapshot.serviceAvailable && (!snapshot.connectivityCheckAvailable || !snapshot.connectivityCheckEnabled))
        status += QStringLiteral("\nConnectivity checks are disabled or unavailable; the reported connectivity state may be cached and does not verify current Internet access.");
    const bool confirming = m_session->awaitingConfirmation();
    m_confirmation->setVisible(confirming);
    m_keep->setVisible(confirming);
    m_undo->setVisible(confirming);
    m_keep->setEnabled(confirming && m_session->remainingSeconds() > 0);
    m_undo->setEnabled(confirming);
    m_confirmation->setText(QStringLiteral("Do you want to keep these network changes? Reverting in %1 seconds.")
        .arg(m_session->remainingSeconds()));
    m_confirmation->setAccessibleName(m_confirmation->text());
    if (m_session->isDirty()) status += QStringLiteral("\nConnection property changes are staged. Open Change adapter settings to apply or revert them.");
    m_status->setText(status);
}
