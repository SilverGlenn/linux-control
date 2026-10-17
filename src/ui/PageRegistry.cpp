#include "PageRegistry.h"

#include <QHash>

namespace {

// Canonical id <-> path table. Keep this the only place these strings live;
// everything else refers to pages by their PageId.
struct Entry {
    PageId  id;
    QString path;
};

const QList<Entry> &table()
{
    static const QList<Entry> kEntries = {
        { PageId::CustomShortcuts, QStringLiteral("Hardware and Sound/Custom Keyboard Shortcuts") },
        { PageId::StandardShortcuts, QStringLiteral("Hardware and Sound/Application Shortcuts") },
        { PageId::Shortcuts, QStringLiteral("Hardware and Sound/Keyboard Shortcuts") },
        { PageId::Keyboard, QStringLiteral("Hardware and Sound/Keyboard") },
        { PageId::DefaultApps, QStringLiteral("Programs/Default Programs") },
        { PageId::LinuxUpdate,      QStringLiteral("System and Security/Linux Update") },
        { PageId::SelectUpdates,
          QStringLiteral("System and Security/Linux Update/Select updates to install") },
        { PageId::ProgramsFeatures, QStringLiteral("Programs/Programs and Features") },
        { PageId::InstalledUpdates,
          QStringLiteral("Programs/Programs and Features/Installed Updates") },
        { PageId::NetworkSharing,
          QStringLiteral("Network and Internet/Network and Sharing Center") },
        { PageId::NetworkConnections, QStringLiteral("Network and Internet/Network Connections") },
        { PageId::Firewall,         QStringLiteral("System and Security/Linux Firewall") },
        { PageId::ActionCenter,     QStringLiteral("System and Security/Action Center") },
        { PageId::PowerOptions,     QStringLiteral("System and Security/Power Options") },
        { PageId::Performance,
          QStringLiteral("System and Security/Performance Information and Tools") },
        { PageId::Personalization,
          QStringLiteral("Appearance and Personalization/Personalization") },
        { PageId::Fonts,            QStringLiteral("Appearance and Personalization/Fonts") },
        { PageId::UserAccounts,
          QStringLiteral("User Accounts and Family Safety/User Accounts") },
        { PageId::EaseOfAccess,     QStringLiteral("Ease of Access/Ease of Access Center") },
        { PageId::DevicesPrinters,  QStringLiteral("Hardware and Sound/Devices and Printers") },
        { PageId::System,           QStringLiteral("System and Security/System") },
        { PageId::NightLight, QStringLiteral("Hardware and Sound/Night Light") },
        { PageId::ScreenLock, QStringLiteral("Appearance and Personalization/Screen Locking") },
        { PageId::AeroLogin, QStringLiteral("Appearance and Personalization/Aero Login and Lock Appearance") },
        { PageId::NightTime, QStringLiteral("Hardware and Sound/Day and Night") },
        { PageId::Display,          QStringLiteral("Hardware and Sound/Screen Resolution") },
        { PageId::WindowBehavior,   QStringLiteral("Appearance and Personalization/Window Behavior") },
        { PageId::DesktopBackground, QStringLiteral("Appearance and Personalization/Desktop Background") },
        { PageId::ThemeComponents,  QStringLiteral("Appearance and Personalization/Appearance Components") },
        { PageId::FontSettings,     QStringLiteral("Appearance and Personalization/Font Settings") },
        { PageId::AeroEffects,      QStringLiteral("Appearance and Personalization/Aero Effects") },
        { PageId::PackagePreview,   QStringLiteral("Programs/Ubuntu Package Preview") },
        { PageId::InputDevices,     QStringLiteral("Hardware and Sound/Mouse and Touchpad") },
    };
    return kEntries;
}

} // namespace

namespace PageRegistry {

QString pathFor(PageId id)
{
    if (id == PageId::Home || id == PageId::None)
        return QString();
    for (const Entry &e : table())
        if (e.id == id)
            return e.path;
    return QString();
}

PageId idForPath(const QString &path)
{
    if (path.isEmpty())
        return PageId::Home;
    for (const Entry &e : table())
        if (e.path == path)
            return e.id;
    return PageId::None;
}

} // namespace PageRegistry
