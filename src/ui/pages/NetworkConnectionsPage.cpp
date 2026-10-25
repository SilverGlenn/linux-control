#include "NetworkConnectionsPage.h"
#include "settings/NetworkSettings.h"
#include "Win7Ui.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTreeWidget>
#include <algorithm>

namespace {
// Keep native checkbox semantics while preventing the desktop's dark indicator
// image rules from leaking into a light Windows property form.
class NetworkCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        QRect box(1, (height() - 13) / 2, 12, 12);
        p.setPen(isEnabled() ? QColor("#7C8994") : QColor("#B0B0B0"));
        p.setBrush(isEnabled() ? QColor("#FAFAFA") : QColor("#EEEEEE"));
        p.drawRect(box);
        if (isChecked()) {
            p.setPen(QPen(isEnabled() ? QColor("#245C31") : QColor("#888888"), 1.8));
            p.drawLine(box.topLeft() + QPoint(2, 6), box.topLeft() + QPoint(5, 9));
            p.drawLine(box.topLeft() + QPoint(5, 9), box.topLeft() + QPoint(10, 3));
        }
        p.setPen(isEnabled() ? QColor("#000000") : QColor("#777777"));
        p.drawText(rect().adjusted(20, 0, 0, 0), Qt::AlignVCenter | Qt::TextShowMnemonic, text());
        Win7::paintFocus(this, p);
    }
};
QString selectedPath(QTreeWidget *tree)
{
    return tree->currentItem() ? tree->currentItem()->data(0, Qt::UserRole).toString() : QString();
}
void selectPath(QTreeWidget *tree, const QString &path)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == path) {
            tree->setCurrentItem(tree->topLevelItem(i)); return;
        }
}
QTreeWidget *list(const QString &name, const QStringList &headers)
{
    auto *tree = new QTreeWidget;
    tree->setObjectName(name);
    tree->setAccessibleName(name);
    tree->setHeaderLabels(headers);
    Win7::configureListTree(tree);
    tree->setStyleSheet(QStringLiteral("QTreeWidget { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; selection-background-color: #CDE6F7; selection-color: #000000; }"));
    tree->setMinimumHeight(130);
    return tree;
}
QPlainTextEdit *textInput(const QString &name, const QString &accessible, bool multiple = false)
{
    auto *edit = new QPlainTextEdit;
    edit->setObjectName(name);
    edit->setAccessibleName(accessible);
    edit->setTabChangesFocus(true);
    edit->setMaximumHeight(multiple ? 48 : 28);
    edit->setMinimumHeight(multiple ? 48 : 28);
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    edit->setStyleSheet(QStringLiteral("QPlainTextEdit { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; selection-background-color: #CDE6F7; selection-color: #000000; } QPlainTextEdit:focus { border-color: #3C7FB1; } QPlainTextEdit:disabled { color: #777777; background: #F0F0F0; }"));
    return edit;
}
void setText(QPlainTextEdit *edit, const QString &value)
{
    if (edit->toPlainText() != value) edit->setPlainText(value);
}
QStringList values(QPlainTextEdit *edit)
{
    return edit->toPlainText().split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
}
QString deviceState(const NetworkDevice &device)
{
    switch (device.state) {
    case 10: return QStringLiteral("Unmanaged");
    case 20: return QStringLiteral("Unavailable");
    case 30: return QStringLiteral("Disconnected");
    case 40: case 50: case 60: case 70: case 80: case 90: return QStringLiteral("Connecting…");
    case 100: return QStringLiteral("Connected");
    case 110: return QStringLiteral("Disconnecting…");
    case 120: return QStringLiteral("Connection failed");
    default: return QStringLiteral("Unknown");
    }
}
bool active(const NetworkDevice *device)
{
    return device && !device->activeConnectionPath.isEmpty() && device->activeConnectionPath != QStringLiteral("/");
}
}

NetworkConnectionsPage::NetworkConnectionsPage(QScrollArea *sidebar, QWidget *parent)
    : NetworkConnectionsPage(sidebar, NetworkSettings::instance(), parent) {}
NetworkConnectionsPage::NetworkConnectionsPage(QScrollArea *sidebar, NetworkSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 800);
    Win7::styleSettingsControls(this);
    layout->addWidget(Win7::pageTitle(QStringLiteral("Network Connections")));
    layout->addSpacing(8);
    layout->addWidget(Win7::bodyLabel(session->connectivityRestrictionReason().isEmpty()
        ? QStringLiteral("Manage Ethernet and Wi-Fi connections. Changes use a timed NetworkManager checkpoint; keep the change when the connection works.")
        : QStringLiteral("Review adapters, nearby wireless networks and saved connections. In this candidate, only inactive saved profiles can be edited; current connections stay read only.")));
    layout->addSpacing(12);
    auto *tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("networkTabs"));
    tabs->setStyleSheet(QStringLiteral("QTabWidget::pane { border: 1px solid #ABADB3; background: #FFFFFF; } QTabBar::tab { color: #000000; background: #F0F0F0; padding: 6px 12px; border: 1px solid #ABADB3; } QTabBar::tab:selected { background: #FFFFFF; border-bottom-color: #FFFFFF; }"));
    layout->addWidget(tabs, 1);

    auto *adapters = new QWidget;
    auto *adapterLayout = new QVBoxLayout(adapters);
    m_devices = list(QStringLiteral("networkDevices"), {QStringLiteral("Adapter"), QStringLiteral("Type"), QStringLiteral("Status"), QStringLiteral("Connection")});
    adapterLayout->addWidget(m_devices, 1);
    m_deviceDetails = Win7::bodyLabel(QString());
    m_deviceDetails->setObjectName(QStringLiteral("networkDeviceDetails"));
    m_deviceDetails->setTextFormat(Qt::PlainText);
    adapterLayout->addWidget(m_deviceDetails);
    adapterLayout->addWidget(Win7::bodyLabel(QStringLiteral("Select an adapter to disconnect it. Use Saved connections to activate an existing profile, or Wireless networks to connect to a nearby access point.")));
    auto *adapterButtons = new QHBoxLayout;
    m_disconnect = new QPushButton(QStringLiteral("&Disconnect"));
    m_disconnect->setObjectName(QStringLiteral("networkDisconnect"));
    m_refresh = new QPushButton(QStringLiteral("&Refresh status"));
    m_refresh->setObjectName(QStringLiteral("networkRefresh"));
    adapterButtons->addWidget(m_disconnect);
    adapterButtons->addStretch();
    adapterButtons->addWidget(m_refresh);
    adapterLayout->addLayout(adapterButtons);
    tabs->addTab(adapters, QStringLiteral("&Adapters"));

    auto *wireless = new QWidget;
    auto *wirelessLayout = new QVBoxLayout(wireless);
    auto *wifiRow = new QHBoxLayout;
    auto *wifiLabel = Win7::label(QStringLiteral("&Wireless adapter:"));
    m_wifiDevice = new QComboBox;
    m_wifiDevice->setObjectName(QStringLiteral("networkWifiDevice"));
    m_wifiDevice->setAccessibleName(QStringLiteral("Wireless adapter"));
    wifiLabel->setBuddy(m_wifiDevice);
    wifiRow->addWidget(wifiLabel);
    wifiRow->addWidget(m_wifiDevice, 1);
    m_scan = new QPushButton(QStringLiteral("&Scan"));
    m_scan->setObjectName(QStringLiteral("networkScan"));
    wifiRow->addWidget(m_scan);
    wirelessLayout->addLayout(wifiRow);
    m_accessPoints = list(QStringLiteral("networkAccessPoints"), {QStringLiteral("Network name"), QStringLiteral("Signal"), QStringLiteral("Security")});
    wirelessLayout->addWidget(m_accessPoints, 1);
    wirelessLayout->addWidget(Win7::bodyLabel(QStringLiteral("Scanning updates the nearby network list. WPA/WPA2 personal and WPA3 personal are supported. Enterprise Wi-Fi and WEP are unsupported.")));
    m_wifiConnect = new QPushButton(QStringLiteral("&Connect"));
    m_wifiConnect->setObjectName(QStringLiteral("networkWifiConnect"));
    wirelessLayout->addWidget(m_wifiConnect, 0, Qt::AlignRight);
    tabs->addTab(wireless, QStringLiteral("&Wireless networks"));

    auto *saved = new QWidget;
    auto *savedLayout = new QVBoxLayout(saved);
    m_profiles = list(QStringLiteral("networkProfiles"), {QStringLiteral("Connection name"), QStringLiteral("Type"), QStringLiteral("Properties")});
    m_profiles->setMaximumHeight(130);
    savedLayout->addWidget(m_profiles);
    auto *profileRow = new QHBoxLayout;
    auto *deviceLabel = Win7::label(QStringLiteral("Connect &using:"));
    m_profileDevice = new QComboBox;
    m_profileDevice->setObjectName(QStringLiteral("networkProfileDevice"));
    m_profileDevice->setAccessibleName(QStringLiteral("Adapter for saved connection"));
    deviceLabel->setBuddy(m_profileDevice);
    profileRow->addWidget(deviceLabel);
    profileRow->addWidget(m_profileDevice, 1);
    m_profileConnect = new QPushButton(QStringLiteral("Co&nnect"));
    m_profileConnect->setObjectName(QStringLiteral("networkProfileConnect"));
    profileRow->addWidget(m_profileConnect);
    savedLayout->addLayout(profileRow);
    m_profileSupport = Win7::bodyLabel(QString());
    m_profileSupport->setObjectName(QStringLiteral("networkProfileSupport"));
    savedLayout->addWidget(m_profileSupport);
    m_form = new QWidget;
    auto *form = new QVBoxLayout(m_form);
    form->setContentsMargins(0, 0, 0, 0);
    auto *nameRow = new QHBoxLayout;
    m_name = textInput(QStringLiteral("networkProfileName"), QStringLiteral("Connection name"));
    auto *nameLabel = Win7::label(QStringLiteral("Connection na&me:"));
    nameLabel->setBuddy(m_name);
    nameRow->addWidget(nameLabel);
    nameRow->addWidget(m_name, 1);
    m_autoconnect = new NetworkCheckBox(QStringLiteral("Connect &automatically"));
    m_autoconnect->setObjectName(QStringLiteral("networkAutoconnect"));
    nameRow->addWidget(m_autoconnect);
    form->addLayout(nameRow);
    auto *ipTabs = new QTabWidget;
    ipTabs->addTab(buildIpFields(m_ipv4, QStringLiteral("networkIpv4"), QStringLiteral("IPv4")), QStringLiteral("IPv&4"));
    ipTabs->addTab(buildIpFields(m_ipv6, QStringLiteral("networkIpv6"), QStringLiteral("IPv6")), QStringLiteral("IPv&6"));
    form->addWidget(ipTabs);
    savedLayout->addWidget(m_form);
    savedLayout->addStretch();
    tabs->addTab(saved, QStringLiteral("&Saved connections"));

    m_support = Win7::bodyLabel(QString());
    m_support->setObjectName(QStringLiteral("networkSupport"));
    layout->addSpacing(8);
    layout->addWidget(m_support);
    m_status = Win7::bodyLabel(QString());
    m_status->setObjectName(QStringLiteral("networkStatus"));
    m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status);
    m_confirmation = Win7::bodyLabel(QString());
    m_confirmation->setObjectName(QStringLiteral("networkConfirmation"));
    layout->addWidget(m_confirmation);
    auto *buttons = new QHBoxLayout;
    m_keep = new QPushButton(QStringLiteral("&Keep changes"));
    m_keep->setObjectName(QStringLiteral("networkKeep"));
    m_undo = new QPushButton(QStringLiteral("Re&vert now"));
    m_undo->setObjectName(QStringLiteral("networkRevertPreview"));
    m_revert = new QPushButton(QStringLiteral("&Revert"));
    m_revert->setObjectName(QStringLiteral("networkRevert"));
    m_apply = new QPushButton(QStringLiteral("&Apply"));
    m_apply->setObjectName(QStringLiteral("networkApply"));
    buttons->addWidget(m_keep); buttons->addWidget(m_undo); buttons->addStretch();
    buttons->addWidget(m_revert); buttons->addWidget(m_apply);
    layout->addSpacing(8);
    layout->addLayout(buttons);
    connect(m_devices, &QTreeWidget::itemSelectionChanged, this, [this] { if (!m_syncing) updateState(); });
    connect(m_accessPoints, &QTreeWidget::itemSelectionChanged, this, [this] { if (!m_syncing) updateState(); });
    connect(m_wifiDevice, &QComboBox::currentIndexChanged, this, [this] { if (!m_syncing) updateState(); });
    connect(m_profileDevice, &QComboBox::currentIndexChanged, this, [this] { if (!m_syncing) updateState(); });
    connect(m_profiles, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!m_syncing) m_session->selectProfile(selectedPath(m_profiles));
    });
    connect(m_name, &QPlainTextEdit::textChanged, this, &NetworkConnectionsPage::stageForm);
    connect(m_autoconnect, &QCheckBox::toggled, this, &NetworkConnectionsPage::stageForm);
    connect(m_scan, &QPushButton::clicked, this, [this] { m_session->scan(m_wifiDevice->currentData().toString()); });
    connect(m_refresh, &QPushButton::clicked, session, &NetworkSettings::refresh);
    connect(m_disconnect, &QPushButton::clicked, this, [this] { m_session->disconnectDevice(selectedDevice()); });
    connect(m_wifiConnect, &QPushButton::clicked, this, &NetworkConnectionsPage::connectWifi);
    connect(m_profileConnect, &QPushButton::clicked, this, &NetworkConnectionsPage::activateProfile);
    connect(m_apply, &QPushButton::clicked, session, &NetworkSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &NetworkSettings::revert);
    connect(m_keep, &QPushButton::clicked, session, &NetworkSettings::keep);
    connect(m_undo, &QPushButton::clicked, session, &NetworkSettings::revert);
    connect(session, &SettingsSession::stateChanged, this, &NetworkConnectionsPage::updateState);
    updateState();
}

QWidget *NetworkConnectionsPage::buildIpFields(IpFields &fields, const QString &prefix, const QString &family)
{
    auto *widget = new QWidget;
    auto *grid = new QGridLayout(widget);
    fields.method = new QComboBox;
    fields.method->setObjectName(prefix + QStringLiteral("Method"));
    fields.method->setAccessibleName(family + QStringLiteral(" address method"));
    fields.method->addItem(QStringLiteral("Automatic (DHCP / router advertisement)"), QStringLiteral("auto"));
    fields.method->addItem(QStringLiteral("Manual"), QStringLiteral("manual"));
    fields.addresses = textInput(prefix + QStringLiteral("Addresses"), family + QStringLiteral(" addresses with prefix"), true);
    fields.addresses->setPlaceholderText(family == QStringLiteral("IPv4") ? QStringLiteral("192.168.1.10/24") : QStringLiteral("2001:db8::10/64"));
    fields.gateway = textInput(prefix + QStringLiteral("Gateway"), family + QStringLiteral(" gateway"));
    fields.dns = textInput(prefix + QStringLiteral("Dns"), family + QStringLiteral(" DNS servers"));
    fields.ignoreAutoDns = new NetworkCheckBox(QStringLiteral("Use only the DNS servers entered &above"));
    fields.ignoreAutoDns->setObjectName(prefix + QStringLiteral("IgnoreAutoDns"));
    const QList<QWidget *> controls{fields.method, fields.addresses, fields.gateway, fields.dns};
    const QStringList labels{QStringLiteral("Address &method:"), QStringLiteral("&Addresses / prefix:"), QStringLiteral("&Gateway:"), QStringLiteral("&DNS servers:")};
    for (int i = 0; i < controls.size(); ++i) {
        auto *label = Win7::label(labels[i]); label->setBuddy(controls[i]);
        grid->addWidget(label, i, 0); grid->addWidget(controls[i], i, 1);
    }
    grid->addWidget(fields.ignoreAutoDns, 4, 1);
    grid->addWidget(Win7::bodyLabel(QStringLiteral("Separate multiple addresses or DNS servers with spaces or commas. Routes and advanced security settings are preserved.")), 5, 0, 1, 2);
    grid->setColumnStretch(1, 1);
    connect(fields.method, &QComboBox::currentIndexChanged, this, &NetworkConnectionsPage::stageForm);
    for (auto *edit : {fields.addresses, fields.gateway, fields.dns})
        connect(edit, &QPlainTextEdit::textChanged, this, &NetworkConnectionsPage::stageForm);
    connect(fields.ignoreAutoDns, &QCheckBox::toggled, this, &NetworkConnectionsPage::stageForm);
    return widget;
}
SettingsSession *NetworkConnectionsPage::settingsSession() const { return m_session; }
QList<SidebarLink> NetworkConnectionsPage::sidebarLinks()
{
    return {Nav::to(QStringLiteral("Network and Sharing Center"), PageId::NetworkSharing)};
}
QList<SidebarLink> NetworkConnectionsPage::sidebarSeeAlso()
{
    return {Nav::to(QStringLiteral("Linux Firewall"), PageId::Firewall)};
}
QString NetworkConnectionsPage::selectedDevice() const { return selectedPath(m_devices); }
void NetworkConnectionsPage::stageForm()
{
    if (m_syncing) return;
    auto preferences = m_session->stagedPreferences();
    preferences.id = m_name->toPlainText();
    preferences.autoconnect = m_autoconnect->isChecked();
    auto read = [](const IpFields &fields) {
        NetworkIpConfig config;
        config.method = fields.method->currentData().toString();
        config.addresses = values(fields.addresses);
        config.gateway = fields.gateway->toPlainText().trimmed();
        config.dns = values(fields.dns);
        config.ignoreAutoDns = fields.ignoreAutoDns->isChecked();
        return config;
    };
    preferences.ipv4 = read(m_ipv4); preferences.ipv6 = read(m_ipv6);
    m_staging = true;
    m_session->stagePreferences(preferences);
    m_staging = false;
}
void NetworkConnectionsPage::activateProfile()
{
    m_session->activateProfile(m_session->selectedProfile(), m_profileDevice->currentData().toString());
}
void NetworkConnectionsPage::connectWifi()
{
    const QString path = selectedPath(m_accessPoints);
    const auto *ap = m_session->snapshot().accessPoint(path);
    if (!ap || ap->security == NetworkAccessPoint::Security::Unsupported) return;
    if (ap->security == NetworkAccessPoint::Security::Open) {
        m_session->connectWifi(path, QString()); return;
    }
    // Credentials stay in this transient dialog and the operation request. They
    // are never written to settings, logs, diagnostics, or the clipboard.
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("networkPasswordDialog"));
    dialog.setWindowTitle(QStringLiteral("Connect to a wireless network"));
    Win7::styleSettingsDialog(&dialog);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = Win7::bodyLabel(QStringLiteral("Enter the security key for %1.").arg(ap->name));
    description->setTextFormat(Qt::PlainText); layout->addWidget(description);
    auto *password = new QLineEdit;
    password->setObjectName(QStringLiteral("networkPassword"));
    password->setAccessibleName(QStringLiteral("Network security key"));
    password->setEchoMode(QLineEdit::Password);
    password->setContextMenuPolicy(Qt::NoContextMenu);
    password->setMinimumWidth(320);
    auto *label = Win7::label(QStringLiteral("&Security key:"));
    label->setBuddy(password); layout->addWidget(label); layout->addWidget(password);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons); password->setFocus();
    const bool accepted = dialog.exec() == QDialog::Accepted;
    QString secret = password->text(); password->clear();
    if (accepted) m_session->connectWifi(path, secret);
    secret.fill(QChar()); secret.clear();
}

void NetworkConnectionsPage::updateState()
{
    if (m_syncing) return;
    m_syncing = true;
    const auto &snapshot = m_session->snapshot();
    const QString adapterPath = selectedPath(m_devices);
    const QString apPath = selectedPath(m_accessPoints);
    const QString wifiPath = m_wifiDevice->currentData().toString();
    const QString profileDevicePath = m_profileDevice->currentData().toString();
    m_devices->clear(); m_wifiDevice->clear();
    for (const auto &device : snapshot.devices) {
        auto *row = new QTreeWidgetItem(m_devices);
        row->setData(0, Qt::UserRole, device.path);
        row->setText(0, device.interfaceName);
        row->setIcon(0, themeIcon(device.type == 2 ? std::initializer_list<const char *>{"network-wireless"} : std::initializer_list<const char *>{"network-wired"}));
        row->setText(1, device.type == 2 ? QStringLiteral("Wi-Fi") : device.type == 1 ? QStringLiteral("Ethernet") : QStringLiteral("Other (read only)"));
        row->setText(2, deviceState(device));
        const auto *profile = snapshot.profile(device.activeProfilePath);
        row->setText(3, profile ? profile->id : QString());
        row->setToolTip(0, device.reason + QStringLiteral("\n") + device.ipv4Addresses.join(QStringLiteral(", ")) + QStringLiteral("\n") + device.ipv6Addresses.join(QStringLiteral(", ")));
        Win7::setItemPointSize(row);
        if (device.type == 2) m_wifiDevice->addItem(device.interfaceName, device.path);
    }
    selectPath(m_devices, adapterPath);
    if (!m_devices->currentItem() && m_devices->topLevelItemCount()) m_devices->setCurrentItem(m_devices->topLevelItem(0));
    const int wifiIndex = m_wifiDevice->findData(wifiPath);
    if (wifiIndex >= 0) m_wifiDevice->setCurrentIndex(wifiIndex);
    m_accessPoints->clear();
    for (const auto &ap : snapshot.accessPoints) {
        if (ap.devicePath != m_wifiDevice->currentData().toString()) continue;
        auto *row = new QTreeWidgetItem(m_accessPoints);
        row->setData(0, Qt::UserRole, ap.path);
        row->setText(0, ap.name);
        row->setText(1, QStringLiteral("%1%").arg(ap.strength));
        row->setText(2, NetworkTypes::securityName(ap.security));
        row->setToolTip(0, ap.reason);
        Win7::setItemPointSize(row);
    }
    selectPath(m_accessPoints, apPath);
    m_profiles->clear();
    for (const auto &profile : snapshot.profiles) {
        auto *row = new QTreeWidgetItem(m_profiles);
        row->setData(0, Qt::UserRole, profile.path);
        row->setText(0, profile.id);
        row->setText(1, profile.type == QStringLiteral("802-3-ethernet") ? QStringLiteral("Ethernet")
            : profile.type == QStringLiteral("802-11-wireless") ? QStringLiteral("Wi-Fi") : profile.type);
        const bool withheldActive = !m_session->connectivityRestrictionReason().isEmpty()
            && std::any_of(snapshot.devices.cbegin(),snapshot.devices.cend(),[&profile](const NetworkDevice &device) { return device.activeProfilePath==profile.path; });
        row->setText(2, withheldActive ? QStringLiteral("Read only in this candidate") : profile.editable ? QStringLiteral("Editable") : QStringLiteral("Unsupported"));
        row->setToolTip(0, withheldActive ? m_session->connectivityRestrictionReason() : profile.reason); Win7::setItemPointSize(row);
    }
    selectPath(m_profiles, m_session->selectedProfile());
    const auto *profile = snapshot.profile(m_session->selectedProfile());
    m_profileDevice->clear();
    for (const auto &device : snapshot.devices)
        if (profile && device.managed && (device.type == 1 || device.type == 2) && device.availableProfilePaths.contains(profile->path))
            m_profileDevice->addItem(device.interfaceName, device.path);
    const int profileDeviceIndex = m_profileDevice->findData(profileDevicePath);
    if (profileDeviceIndex >= 0) m_profileDevice->setCurrentIndex(profileDeviceIndex);
    const auto &preferences = m_session->stagedPreferences();
    if (!m_staging) { setText(m_name, preferences.id); m_autoconnect->setChecked(preferences.autoconnect); }
    auto write = [this](IpFields &fields, const NetworkIpConfig &config) {
        if (!m_staging) {
            fields.method->setCurrentIndex(fields.method->findData(config.method));
            setText(fields.addresses, config.addresses.join(QStringLiteral("\n")));
            setText(fields.gateway, config.gateway);
            setText(fields.dns, config.dns.join(QStringLiteral(", ")));
            fields.ignoreAutoDns->setChecked(config.ignoreAutoDns);
        }
        fields.addresses->setEnabled(config.method == QStringLiteral("manual"));
        fields.gateway->setEnabled(config.method == QStringLiteral("manual"));
    };
    write(m_ipv4, preferences.ipv4); write(m_ipv6, preferences.ipv6);
    const bool idle = m_session->isReady() && !m_session->isBusy() && !m_session->isScanning();
    const bool available = idle && snapshot.serviceAvailable;
    const QString connectivityRestriction = m_session->connectivityRestrictionReason();
    const QString profileRestriction = m_session->selectedProfileEditReason();
    const bool action = available && !m_session->isDirty() && !m_session->hasConflict() && m_session->rollbackAvailable() && connectivityRestriction.isEmpty();
    m_profiles->setEnabled(available && !m_session->isDirty());
    m_form->setEnabled(available && profile && profile->editable && !m_session->hasConflict() && profileRestriction.isEmpty());
    m_profileDevice->setEnabled(action && m_profileDevice->count() > 0);
    NetworkRequest activation; activation.action = NetworkRequest::Action::ActivateProfile;
    activation.profilePath = m_session->selectedProfile(); activation.devicePath = m_profileDevice->currentData().toString();
    QString activationReason;
    m_profileConnect->setEnabled(action && profile && m_profileDevice->count() > 0
        && NetworkTypes::validateRequest(snapshot, activation, activationReason));
    m_profileConnect->setToolTip(connectivityRestriction.isEmpty() ? activationReason : connectivityRestriction);
    const auto *wifiDevice = snapshot.device(m_wifiDevice->currentData().toString());
    const bool wifiEnabled = snapshot.networkingEnabled && snapshot.wirelessEnabled && snapshot.wirelessHardwareEnabled;
    m_scan->setEnabled(available && wifiEnabled && wifiDevice && wifiDevice->managed);
    m_scan->setToolTip(wifiEnabled ? QStringLiteral("Scan for nearby wireless networks.") : QStringLiteral("Wi-Fi is disabled by NetworkManager or a hardware switch."));
    m_wifiDevice->setEnabled(available);
    const auto *ap = snapshot.accessPoint(selectedPath(m_accessPoints));
    m_wifiConnect->setEnabled(action && wifiEnabled && wifiDevice && wifiDevice->managed && ap && ap->security != NetworkAccessPoint::Security::Unsupported);
    m_wifiConnect->setToolTip(!connectivityRestriction.isEmpty() ? connectivityRestriction : ap ? ap->reason : QStringLiteral("Select a wireless network."));
    const auto *selectedAdapter = snapshot.device(selectedDevice());
    m_deviceDetails->setText(selectedAdapter ? QStringLiteral("%1%2%3").arg(
        selectedAdapter->reason,
        selectedAdapter->ipv4Addresses.isEmpty() ? QString() : QStringLiteral("\nIPv4: ") + selectedAdapter->ipv4Addresses.join(QStringLiteral(", ")),
        selectedAdapter->ipv6Addresses.isEmpty() ? QString() : QStringLiteral("\nIPv6: ") + selectedAdapter->ipv6Addresses.join(QStringLiteral(", "))) : QString());
    if (selectedAdapter && selectedAdapter->type != 1 && selectedAdapter->type != 2 && m_deviceDetails->text().isEmpty())
        m_deviceDetails->setText(QStringLiteral("This adapter type is read only. Only managed Ethernet and Wi-Fi adapters can be changed here."));
    m_disconnect->setEnabled(action && selectedAdapter && selectedAdapter->managed
        && (selectedAdapter->type == 1 || selectedAdapter->type == 2) && active(selectedAdapter));
    m_disconnect->setToolTip(connectivityRestriction);
    m_refresh->setEnabled(!m_session->isBusy() && !m_session->isScanning() && !m_session->isDirty());
    const bool selectedProfileActive = profile && std::any_of(snapshot.devices.cbegin(), snapshot.devices.cend(), [profile](const NetworkDevice &device) { return device.activeProfilePath == profile->path; });
    m_apply->setEnabled(available && m_session->isDirty() && !m_session->hasConflict()
        && (!selectedProfileActive || m_session->rollbackAvailable()) && profile && profile->editable && profileRestriction.isEmpty());
    m_revert->setEnabled(!m_session->isBusy() && !m_session->isScanning()
        && (m_session->isDirty() || m_session->hasConflict()));
    m_profileSupport->setText(profileRestriction.isEmpty() && profile ? profile->reason : profileRestriction);
    m_profileSupport->setVisible(!m_profileSupport->text().isEmpty());
    m_support->setText(m_session->supportText());
    m_support->setVisible(!m_support->text().isEmpty());
    const bool confirmation = m_session->awaitingConfirmation();
    m_confirmation->setVisible(confirmation); m_keep->setVisible(confirmation); m_undo->setVisible(confirmation);
    m_keep->setEnabled(confirmation && m_session->remainingSeconds() > 0);
    m_undo->setEnabled(confirmation);
    m_confirmation->setText(QStringLiteral("Do you want to keep these network changes? Reverting in %1 seconds.").arg(m_session->remainingSeconds()));
    m_status->setText(!m_session->errorText().isEmpty() ? m_session->errorText()
        : m_session->isScanning() ? QStringLiteral("Scanning for wireless networks…")
        : m_session->isBusy() && !confirmation ? QStringLiteral("Waiting for NetworkManager…")
        : m_session->isDirty() ? QStringLiteral("Connection properties have changed. Click Apply to test the changes, or Revert to discard them.") : m_session->resultText());
    m_status->setStyleSheet(m_session->errorText().isEmpty() ? QStringLiteral("color: #333333;") : QStringLiteral("color: #8A1C1C;"));
    m_status->setVisible(!m_status->text().isEmpty());
    m_syncing = false;
}
