#include "ActionCatalog.h"
#include "Commands.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>

namespace {
QString normalized(const QString &text)
{
    QString result;
    for (const QChar ch : text.normalized(QString::NormalizationForm_D).toCaseFolded()) {
        const auto category = ch.category();
        if (category != QChar::Mark_NonSpacing && category != QChar::Mark_SpacingCombining
            && category != QChar::Mark_Enclosing)
            result += ch;
    }
    return result.simplified();
}

bool sameTarget(const LinkTarget &a, const LinkTarget &b)
{
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case LinkTarget::Page: return a.page == b.page;
    case LinkTarget::Command: return a.command == b.command;
    case LinkTarget::Applet: return a.applet == b.applet;
    case LinkTarget::Callback: return a.callback == b.callback;
    case LinkTarget::Home: return true;
    case LinkTarget::None: return false;
    }
    return false;
}

// Inspect installed metadata only. Availability must never launch kcmshell or
// a settings provider, since search and view construction call it frequently.
struct ModuleMetadata {
    bool known = false;
    QSet<QString> modules;
};
const ModuleMetadata &moduleMetadata()
{
    static const ModuleMetadata metadata = [] {
        ModuleMetadata result;
        QStringList roots = QCoreApplication::libraryPaths();
        roots.append(QLibraryInfo::path(QLibraryInfo::PluginsPath));
        roots.removeDuplicates();
        for (const QString &root : roots) {
            for (const QString &subdir : {QStringLiteral("/plasma/kcms/systemsettings"),
                                          QStringLiteral("/plasma/kcms/systemsettings_qwidgets"),
                                          QStringLiteral("/plasma/kcms")}) {
                const QDir dir(root + subdir);
                const auto entries = dir.entryList({QStringLiteral("kcm_*.so"),
                                                   QStringLiteral("kcm_*.dll"),
                                                   QStringLiteral("kcm_*.dylib")}, QDir::Files);
                if (entries.isEmpty()) continue;
                result.known = true;
                for (const QString &entry : entries)
                    result.modules.insert(QFileInfo(entry).completeBaseName());
            }
        }
        return result;
    }();
    return metadata;
}

ActionCatalog::Availability commandAvailability(const QStringList &command)
{
    if (command.isEmpty() || command.first().isEmpty())
        return {false, QStringLiteral("No launcher is configured for this action.")};
    if (QStandardPaths::findExecutable(command.first()).isEmpty())
        return {false, QStringLiteral("Required program '%1' is not installed or is not on PATH.")
                       .arg(command.first())};
    if (command.first() == QLatin1String("kcmshell6") && command.size() > 1) {
        const auto &metadata = moduleMetadata();
        if (metadata.known && !metadata.modules.contains(command.at(1)))
            return {false, QStringLiteral("KDE settings module '%1' is not installed.")
                           .arg(command.at(1))};
    }
    if (command == kGetWidgetsCmd && !QFileInfo::exists(command.at(1)))
        return {false, QStringLiteral("The Plasma widget download configuration is not installed.")};
    return {true, {}};
}
} // namespace

namespace ActionCatalog {
const QList<ActionDefinition> &all()
{
    static const QList<ActionDefinition> definitions = {
        { ActionId::ActionCenter, "Action Center",
          "Review the limited security and maintenance status checks performed by this application.", "software-update-available",
          "System and Security", Nav::to({}, PageId::ActionCenter).target,
          { "Windows Security", "security maintenance", "computer status" }, true },
        { ActionId::ReviewYourComputersStatusAndResolveIssues, "Review your computer's status and resolve issues",
          "Review the limited security and maintenance status checks performed by this application.", "preferences-system",
          "System and Security", Nav::to({}, PageId::ActionCenter).target,
          {  }, false },
        { ActionId::ChangeUserAccountControlSettings, "Change User Account Control settings",
          "Authorization policy settings are not available in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::TroubleshootCommonComputerProblems, "Troubleshoot common computer problems",
          "A troubleshooting wizard is not implemented.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::RestoreYourComputerToAnEarlierTime, "Restore your computer to an earlier time",
          "Restore your computer to an earlier time is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::LinuxFirewall, "Linux Firewall",
          "Open Linux Firewall.", "preferences-security-firewall",
          "System and Security", Nav::to({}, PageId::Firewall).target,
          { "Windows Firewall", "network security", "ufw" }, true },
        { ActionId::CheckFirewallStatus, "Check firewall status",
          "Open Linux Firewall.", "preferences-system",
          "System and Security", Nav::to({}, PageId::Firewall).target,
          {  }, false },
        { ActionId::AllowAProgramThroughLinuxFirewall, "Allow a program through Linux Firewall",
          "Per-program firewall rules are not available in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::System, "System",
          "Open System.", "redhat-system_tools",
          "System and Security", Nav::to({}, PageId::System).target,
          { "computer information", "RAM", "processor", "CPU", "hostname" }, true },
        { ActionId::ViewAmountOfRAMAndProcessorSpeed, "View amount of RAM and processor speed",
          "Open System.", "preferences-system",
          "System and Security", Nav::to({}, PageId::System).target,
          {  }, false },
        { ActionId::CheckTheLinuxExperienceIndex, "Check the Linux Experience Index",
          "Open Performance Information and Tools.", "preferences-system",
          "System and Security", Nav::to({}, PageId::Performance).target,
          {  }, false },
        { ActionId::AllowRemoteAccess, "Allow remote access",
          "Remote access configuration is not implemented.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::SeeTheNameOfThisComputer, "See the name of this computer",
          "Open System.", "preferences-system",
          "System and Security", Nav::to({}, PageId::System).target,
          {  }, false },
        { ActionId::DeviceManager, "Device Manager",
          "Open the device manager.", "preferences-system",
          "System and Security", Nav::command({}, kDeviceManagerCmd).target,
          { "hardware", "devices", "drivers", "devmgmt" }, true },
        { ActionId::LinuxUpdate, "Linux Update",
          "Open Linux Update.", "system-software-update",
          "System and Security", Nav::to({}, PageId::LinuxUpdate).target,
          { "Windows Update", "software update", "package upgrade", "pacman", "Discover" }, true },
        { ActionId::TurnAutomaticUpdatingOnOrOff, "Turn automatic updating on or off",
          "Turn automatic updating on or off is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::CheckForUpdates, "Check for updates",
          "Check package repositories for available updates.", "preferences-system",
          "System and Security", Nav::callback({}, CallbackId::CheckUpdates).target,
          { "Windows Update", "Linux Update", "pacman", "upgrade" }, false },
        { ActionId::ViewInstalledUpdates, "View installed updates",
          "Open Installed Updates.", "preferences-system",
          "System and Security", Nav::to({}, PageId::InstalledUpdates).target,
          {  }, false },
        { ActionId::PowerOptions, "Power Options",
          "Configure native power actions, display and brightness policies, battery behavior and performance profiles.", "preferences-system-power-management",
          "System and Security", Nav::to({}, PageId::PowerOptions).target,
          { "battery", "power plans", "performance profiles", "PowerDevil", "lid", "brightness", "hibernate", "critical battery" }, true },
        { ActionId::RequireAPasswordWhenTheComputerWakes, "Require a password when the computer wakes",
          "Review screen locking after sleep and password requirements.", "preferences-system",
          "System and Security", Nav::to({}, PageId::ScreenLock).target,
          { "resume", "password", "sleep", "screen lock" }, false },
        { ActionId::ChangeWhatThePowerButtonsDo, "Change what the power buttons do",
          "Choose supported power-button and lid actions for each power source.", "preferences-system",
          "System and Security", Nav::to({}, PageId::PowerOptions).target,
          { "lid", "power button", "long press", "sleep", "shutdown" }, false },
        { ActionId::ChangeWhenTheComputerSleeps, "Change when the computer sleeps",
          "Configure when PowerDevil puts the computer to sleep.", "preferences-system",
          "System and Security", Nav::to({}, PageId::PowerOptions).target,
          {  }, false },
        { ActionId::BackupAndRestore, "Backup and Restore",
          "Backup and Restore is not implemented in this Control Panel.", "preferences-system-backup",
          "System and Security", {},
          { "Windows Backup", "restore", "recovery" }, false },
        { ActionId::BackUpYourComputer, "Back up your computer",
          "Back up your computer is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::RestoreFilesFromBackup, "Restore files from backup",
          "Restore files from backup is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::BitLockerDriveEncryption, "BitLocker Drive Encryption",
          "BitLocker Drive Encryption is not implemented in this Control Panel.", "drive-harddisk-encrypted",
          "System and Security", {},
          { "Windows BitLocker", "disk encryption", "LUKS" }, false },
        { ActionId::ProtectYourComputerByEncryptingDataOnYourDisk, "Protect your computer by encrypting data on your disk",
          "Protect your computer by encrypting data on your disk is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::ManageBitLocker, "Manage BitLocker",
          "Manage BitLocker is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::AdministrativeTools, "Administrative Tools",
          "Administrative Tools is not implemented in this Control Panel.", "preferences-desktop-activities",
          "System and Security", {},
          {  }, false },
        { ActionId::FreeUpDiskSpace, "Free up disk space",
          "Free up disk space is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::DefragmentYourHardDrive, "Defragment your hard drive",
          "Defragment your hard drive is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::CreateAndFormatHardDiskPartitions, "Create and format hard disk partitions",
          "Create and format hard disk partitions is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::ViewEventLogs, "View event logs",
          "View event logs is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::ScheduleTasks, "Schedule tasks",
          "Schedule tasks is not implemented in this Control Panel.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::NetworkAndSharingCenter, "Network and Sharing Center",
          "Open Network and Sharing Center.", "preferences-system-network",
          "Network and Internet", Nav::to({}, PageId::NetworkSharing).target,
          { "internet", "connections", "Wi-Fi", "wireless", "Ethernet" }, true },
        { ActionId::ViewNetworkStatusAndTasks, "View network status and tasks",
          "Open Network and Sharing Center.", "preferences-system",
          "Network and Internet", Nav::to({}, PageId::NetworkSharing).target,
          {  }, false },
        { ActionId::ConnectToANetwork, "Connect to a network",
          "Scan for Wi-Fi and connect using native network controls.", "preferences-system",
          "Network and Internet", Nav::to({}, PageId::NetworkConnections).target,
          { "Wi-Fi", "wireless", "SSID", "password", "Ethernet" }, false },
        { ActionId::ViewNetworkComputersAndDevices, "View network computers and devices",
          "View network computers and devices is not implemented in this Control Panel.", "preferences-system",
          "Network and Internet", {},
          {  }, false },
        { ActionId::AddAWirelessDeviceToTheNetwork, "Add a wireless device to the network",
          "Add a wireless device to the network is not implemented in this Control Panel.", "preferences-system",
          "Network and Internet", {},
          {  }, false },
        { ActionId::HomeGroup, "HomeGroup",
          "Windows HomeGroup sharing is not implemented.", "preferences-system-network-share-windows",
          "Network and Internet", {},
          { "Windows HomeGroup", "file sharing", "network share" }, false },
        { ActionId::ChooseHomegroupAndSharingOptions, "Choose homegroup and sharing options",
          "Windows HomeGroup sharing is not implemented.", "preferences-system",
          "Network and Internet", {},
          {  }, false },
        { ActionId::InternetOptions, "Internet Options",
          "Internet Options is not implemented in this Control Panel.", "applications-internet",
          "Network and Internet", {},
          {  }, false },
        { ActionId::ChangeYourHomepage, "Change your homepage",
          "Change your homepage is not implemented in this Control Panel.", "preferences-system",
          "Network and Internet", {},
          {  }, false },
        { ActionId::ManageBrowserAddOns, "Manage browser add-ons",
          "Manage browser add-ons is not implemented in this Control Panel.", "preferences-system",
          "Network and Internet", {},
          {  }, false },
        { ActionId::DeleteBrowsingHistoryAndCookies, "Delete browsing history and cookies",
          "Delete browsing history and cookies is not implemented in this Control Panel.", "preferences-system",
          "Network and Internet", {},
          {  }, false },
        { ActionId::DevicesAndPrinters, "Devices and Printers",
          "Open Devices and Printers.", "preferences-desktop-peripherals",
          "Hardware and Sound", Nav::to({}, PageId::DevicesPrinters).target,
          { "hardware", "printer", "Bluetooth", "USB" }, true },
        { ActionId::AddADevice, "Add a device",
          "Open Devices and Printers.", "preferences-system",
          "Hardware and Sound", Nav::to({}, PageId::DevicesPrinters).target,
          {  }, false },
        { ActionId::AddAPrinter, "Add a printer",
          "Open Devices and Printers.", "preferences-system",
          "Hardware and Sound", Nav::to({}, PageId::DevicesPrinters).target,
          {  }, false },
        { ActionId::Mouse, "Mouse",
          "Change mouse and touchpad buttons, pointer motion and scrolling.", "input-mouse",
          "Hardware and Sound", Nav::to({}, PageId::InputDevices).target,
          { "KDE", "System Settings", "kcm_mouse", "pointer", "buttons", "peripherals" }, true },
        { ActionId::AutoPlay, "AutoPlay",
          "AutoPlay is not implemented in this Control Panel.", "exec",
          "Hardware and Sound", {},
          {  }, false },
        { ActionId::ChangeDefaultSettingsForMediaOrDevices, "Change default settings for media or devices",
          "Removable-media and AutoPlay settings are not implemented.", "preferences-system",
          "Hardware and Sound", {},
          {  }, false },
        { ActionId::PlayCDsOrOtherMediaAutomatically, "Play CDs or other media automatically",
          "Removable-media and AutoPlay settings are not implemented.", "preferences-system",
          "Hardware and Sound", {},
          {  }, false },
        { ActionId::Sound, "Sound",
          "Open the Sound dialog.", "preferences-desktop-sound",
          "Hardware and Sound", Nav::applet({}, "sound").target,
          { "audio", "speaker", "microphone", "volume", "PulseAudio", "PipeWire" }, true },
        { ActionId::AdjustSystemVolume, "Adjust system volume",
          "Change playback volume and mute through the native Sound dialog.", "preferences-system",
          "Hardware and Sound", Nav::applet({}, "sound").target,
          { "KDE", "audio", "volume", "PulseAudio", "PipeWire" }, false },
        { ActionId::ChangeSystemSounds, "Change system sounds",
          "Sound event configuration is not implemented in this Control Panel.", "preferences-system",
          "Hardware and Sound", {},
          {  }, false },
        { ActionId::ManageAudioDevices, "Manage audio devices",
          "Change default devices, ports, sound-card profiles and channel levels.", "preferences-system",
          "Hardware and Sound", Nav::applet({}, "sound").target,
          {  }, false },
        { ActionId::ChangePowerSavingSettings, "Change power-saving settings",
          "Choose a performance or power-saving profile supported by this computer.", "preferences-system",
          "Hardware and Sound", Nav::to({}, PageId::PowerOptions).target,
          {  }, false },
        { ActionId::ChooseAPowerPlan, "Choose a power plan",
          "Choose a performance or power-saving profile supported by this computer.", "preferences-system",
          "Hardware and Sound", Nav::to({}, PageId::PowerOptions).target,
          {  }, false },
        { ActionId::Display, "Display",
          "Arrange displays and choose supported resolution, scaling and orientation with automatic rollback.", "preferences-desktop-display",
          "Hardware and Sound", Nav::to({}, PageId::Display).target,
          { "KDE", "System Settings", "kcm_kscreen", "resolution", "scaling", "monitor", "screen", "refresh rate", "orientation", "rotation", "arrangement", "external display" }, true },
        { ActionId::MakeTextAndOtherItemsLargerOrSmaller, "Make text and other items larger or smaller",
          "Choose a supported scale for each connected display.", "preferences-desktop-settings",
          "Hardware and Sound", Nav::to({}, PageId::Display).target,
          { "KDE", "System Settings", "kcm_kscreen" }, false },
        { ActionId::AdjustScreenResolution, "Adjust screen resolution",
          "Choose a supported screen resolution and refresh rate with automatic rollback.", "preferences-desktop-settings",
          "Hardware and Sound", Nav::to({}, PageId::Display).target,
          { "KDE", "System Settings", "kcm_kscreen" }, false },
        { ActionId::ConnectToAnExternalDisplay, "Connect to an external display",
          "Enable and arrange connected displays using native Aero controls.", "preferences-desktop-settings",
          "Hardware and Sound", Nav::to({}, PageId::Display).target,
          { "KDE", "System Settings", "kcm_kscreen" }, false },
        { ActionId::HowToCorrectMonitorFlickerRefreshRate, "How to correct monitor flicker (refresh rate)",
          "How to correct monitor flicker (refresh rate) is not implemented in this Control Panel.", "preferences-system",
          "Hardware and Sound", {},
          {  }, false },
        { ActionId::ProgramsAndFeatures, "Programs and Features",
          "Open Programs and Features.", "application-vnd.debian.binary-package",
          "Programs", Nav::to({}, PageId::ProgramsFeatures).target,
          { "Add Remove Programs", "installed applications", "uninstall software", "packages" }, true },
        { ActionId::UninstallAProgram, "Uninstall a program",
          "Open Programs and Features.", "preferences-system",
          "Programs", Nav::to({}, PageId::ProgramsFeatures).target,
          {  }, false },
        { ActionId::TurnLinuxFeaturesOnOrOff, "Turn Linux features on or off",
          "Turn Linux features on or off is not implemented in this Control Panel.", "preferences-system",
          "Programs", {},
          {  }, false },
        { ActionId::RunProgramsMadeForPreviousVersionsOfLinux, "Run programs made for previous versions of Linux",
          "Run programs made for previous versions of Linux is not implemented in this Control Panel.", "preferences-system",
          "Programs", {},
          {  }, false },
        { ActionId::HowToInstallAProgram, "How to install a program",
          "How to install a program is not implemented in this Control Panel.", "preferences-system",
          "Programs", {},
          {  }, false },
        { ActionId::DefaultPrograms, "Default Programs",
          "Choose default applications for twelve common roles and supported file types.", "preferences-desktop-default-applications",
          "Programs", Nav::to({}, PageId::DefaultApps).target,
          { "KDE", "System Settings", "kcm_componentchooser", "default apps", "file associations", "browser", "email" }, true },
        { ActionId::MakeAFileTypeAlwaysOpenInASpecificProgram, "Make a file type always open in a specific program",
          "Choose a compatible installed application for a file type or protocol.", "preferences-desktop-settings",
          "Programs", Nav::to({}, PageId::DefaultApps).target,
          { "KDE", "System Settings", "kcm_filetypes" }, true },
        { ActionId::SetYourDefaultPrograms, "Set your default programs",
          "Choose default applications for twelve common roles and supported file types.", "preferences-desktop-settings",
          "Programs", Nav::to({}, PageId::DefaultApps).target,
          { "KDE", "System Settings", "kcm_componentchooser" }, false },
        { ActionId::DesktopGadgets, "Desktop Gadgets",
          "Open Plasma widgets.", "gadgets",
          "Programs", Nav::command({}, kWidgetExplorerCmd).target,
          { "KDE", "Plasma", "widgets" }, true },
        { ActionId::AddGadgetsToTheDesktop, "Add gadgets to the desktop",
          "Open Plasma widgets.", "preferences-system",
          "Programs", Nav::command({}, kWidgetExplorerCmd).target,
          { "KDE", "Plasma", "widgets" }, false },
        { ActionId::GetMoreGadgetsOnline, "Get more gadgets online",
          "Open Plasma widgets.", "preferences-system",
          "Programs", Nav::command({}, kGetWidgetsCmd).target,
          { "KDE", "Plasma", "widgets" }, false },
        { ActionId::UninstallAGadget, "Uninstall a gadget",
          "Removing a specific Plasma widget is not implemented.", "preferences-system",
          "Programs", {},
          {  }, false },
        { ActionId::RestoreDesktopGadgetsInstalledWithLinux, "Restore desktop gadgets installed with Linux",
          "Restoring default Plasma widgets is not implemented.", "preferences-system",
          "Programs", {},
          {  }, false },
        { ActionId::UserAccounts, "User Accounts",
          "View the current account and edit its display name through AccountsService.", "system-users",
          "User Accounts and Family Safety", Nav::to({}, PageId::UserAccounts).target,
          { "users", "password", "account picture", "administrator" }, true },
        { ActionId::ChangeYourAccountPicture, "Change your account picture",
          "Open KDE's user editor to choose the account picture (development bridge).", "preferences-system",
          "User Accounts and Family Safety", Nav::command({}, kcm("kcm_users")).target,
          {  }, false },
        { ActionId::AddOrRemoveUserAccounts, "Add or remove user accounts",
          "Open KDE's user editor for account administration (development bridge).", "preferences-system",
          "User Accounts and Family Safety", Nav::command({}, kcm("kcm_users")).target,
          {  }, false },
        { ActionId::ChangeYourLinuxPassword, "Change your Linux Password",
          "Open KDE's user editor for password changes (development bridge).", "preferences-system",
          "User Accounts and Family Safety", Nav::command({}, kcm("kcm_users")).target,
          {  }, false },
        { ActionId::CredentialManager, "Credential Manager",
          "Manage password wallets in KDE System Settings.", "preferences-system-login",
          "User Accounts and Family Safety", Nav::command({}, kcm("kcm_kwallet5")).target,
          { "KDE", "System Settings", "kcm_kwallet5", "Windows Credentials", "KWallet", "wallet", "passwords" }, true },
        { ActionId::ManageLinuxCredentials, "Manage Linux Credentials",
          "Manage password wallets in KDE System Settings.", "preferences-desktop-settings",
          "User Accounts and Family Safety", Nav::command({}, kcm("kcm_kwallet5")).target,
          { "KDE", "System Settings", "kcm_kwallet5" }, false },
        { ActionId::Personalization, "Personalization",
          "Customize the approved Aero glass color, transparency, blur, and pointers.", "preferences-desktop-theme",
          "Appearance and Personalization", Nav::to({}, PageId::Personalization).target,
          { "wallpaper", "desktop theme", "Aero", "appearance" }, true },
        { ActionId::ChangeWindowColors, "Change window colors",
          "Customize the approved Aero glass color, transparency, blur, and pointers.", "preferences-system",
          "Appearance and Personalization", Nav::to({}, PageId::Personalization).target,
          {  }, false },
        { ActionId::ChangeDesktopBackground, "Change desktop background",
          "Choose a picture and position for one Plasma desktop.", "preferences-desktop-theme",
          "Appearance and Personalization", Nav::to({}, PageId::DesktopBackground).target,
          { "KDE", "System Settings", "kcm_wallpaper", "wallpaper", "Plasma" }, true },
        { ActionId::ChangeWindowGlassColors, "Change window glass colors",
          "Customize the glass color of the approved Aero theme.", "preferences-desktop-theme",
          "Appearance and Personalization", Nav::to({}, PageId::Personalization).target,
          { "KDE", "System Settings", "kcm_colors", "Aero", "colour", "color scheme" }, true },
        { ActionId::ChangeSoundEffects, "Change sound effects",
          "Sound event configuration is not implemented in this Control Panel.", "preferences-system",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::ChangeScreenLockSettings, "Change screen lock settings",
          "Choose idle locking, resume locking and password grace in the Aero Control Panel.", "preferences-desktop-settings",
          "Appearance and Personalization", Nav::to({}, PageId::ScreenLock).target,
          { "KDE", "kcm_screenlocker", "screen locker", "lock screen", "idle timeout", "password grace", "resume", "Aero" }, true },
        { ActionId::TaskbarAndStartMenu, "Taskbar and Start Menu",
          "Taskbar and Start Menu is not implemented in this Control Panel.", "preferences-desktop-plasma-theme",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::CustomizeTheStartMenu, "Customize the Start menu",
          "Customize the Start menu is not implemented in this Control Panel.", "preferences-system",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::CustomizeIconsOnTheTaskbar, "Customize icons on the taskbar",
          "Customize icons on the taskbar is not implemented in this Control Panel.", "preferences-system",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::ChangeThePictureOnTheStartMenu, "Change the picture on the Start menu",
          "Change the picture on the Start menu is not implemented in this Control Panel.", "preferences-system",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::EaseOfAccessCenter, "Ease of Access Center",
          "Open Ease of Access Center.", "preferences-desktop-accessibility",
          "Appearance and Personalization", Nav::to({}, PageId::EaseOfAccess).target,
          { "accessibility", "a11y", "screen reader", "magnifier", "high contrast" }, true },
        { ActionId::AccommodateLowVision, "Accommodate low vision",
          "Accommodate low vision is not implemented in this Control Panel.", "preferences-system",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::UseScreenReader, "Use screen reader",
          "Use screen reader is not implemented in this Control Panel.", "preferences-system",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::TurnOnEasyAccessKeys, "Turn on easy access keys",
          "Turn on easy access keys is not implemented in this Control Panel.", "preferences-system",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::TurnHighContrastOnOrOff, "Turn High Contrast on or off",
          "Turn High Contrast on or off is not implemented in this Control Panel.", "preferences-system",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::FolderOptions, "Folder Options",
          "Folder Options is not implemented in this Control Panel.", "folder-templates",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::SpecifySingleOrDoubleClickToOpen, "Specify single- or double-click to open",
          "Specify single- or double-click to open is not implemented in this Control Panel.", "preferences-system",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::ShowHiddenFilesAndFolders, "Show hidden files and folders",
          "Show hidden files and folders is not implemented in this Control Panel.", "preferences-system",
          "Appearance and Personalization", {},
          {  }, false },
        { ActionId::Fonts, "Fonts",
          "Preview installed fonts and manage fonts installed for your account by Control Panel.", "preferences-desktop-font-installer",
          "Appearance and Personalization", Nav::to({}, PageId::Fonts).target,
          { "typefaces", "font viewer", "typography" }, true },
        { ActionId::PreviewInstalledFonts, "Preview installed fonts",
          "Open Fonts.", "preferences-system",
          "Appearance and Personalization", Nav::to({}, PageId::Fonts).target,
          {  }, false },
        { ActionId::ChangeFontSettings, "Change Font Settings",
          "Configure KDE interface fonts, antialiasing, hinting and subpixel rendering.", "preferences-desktop-settings",
          "Appearance and Personalization", Nav::to({}, PageId::FontSettings).target,
          { "KDE", "kcm_fonts", "font rendering", "font size", "hinting", "typography", "subpixel" }, true },
        { ActionId::AdjustClearTypeText, "Adjust ClearType text",
          "Configure KDE text rendering, including antialiasing and subpixel order.", "preferences-desktop-settings",
          "Appearance and Personalization", Nav::to({}, PageId::FontSettings).target,
          { "KDE", "System Settings", "kcm_fonts", "font rendering", "antialiasing" }, false },
        { ActionId::DateAndTime, "Date and Time",
          "Open the Date and Time dialog.", "preferences-system-time",
          "Clock, Language, and Region", Nav::applet({}, "datetime").target,
          { "clock", "timezone", "time zone", "NTP", "Chrony", "chronyd", "systemd-timesyncd", "timesyncd", "time synchronization", "Internet Time" }, true },
        { ActionId::SetTheTimeAndDate, "Set the time and date",
          "Open the Date and Time dialog.", "preferences-system",
          "Clock, Language, and Region", Nav::applet({}, "datetime").target,
          {  }, false },
        { ActionId::ChangeTheTimeZone, "Change the time zone",
          "Open the Date and Time dialog.", "preferences-system",
          "Clock, Language, and Region", Nav::applet({}, "datetime").target,
          {  }, false },
        { ActionId::AddClocksForDifferentTimeZones, "Add clocks for different time zones",
          "Additional clock configuration is not implemented in this Control Panel.", "preferences-system",
          "Clock, Language, and Region", {},
          {  }, false },
        { ActionId::AddTheClockGadgetToTheDesktop, "Add the Clock gadget to the desktop",
          "Adding a specific Clock widget is not implemented.", "preferences-system",
          "Clock, Language, and Region", {},
          {  }, false },
        { ActionId::RegionAndLanguage, "Region and Language",
          "Configure languages and regional formats in KDE System Settings.", "preferences-desktop-locale",
          "Clock, Language, and Region", Nav::command({}, kcm("kcm_regionandlang")).target,
          { "KDE", "System Settings", "kcm_regionandlang", "locale", "language", "formats", "keyboard layout" }, true },
        { ActionId::InstallOrUninstallDisplayLanguages, "Install or uninstall display languages",
          "Configure languages and regional formats in KDE System Settings.", "preferences-desktop-settings",
          "Clock, Language, and Region", Nav::command({}, kcm("kcm_regionandlang")).target,
          { "KDE", "System Settings", "kcm_regionandlang" }, false },
        { ActionId::ChangeDisplayLanguage, "Change display language",
          "Configure languages and regional formats in KDE System Settings.", "preferences-desktop-settings",
          "Clock, Language, and Region", Nav::command({}, kcm("kcm_regionandlang")).target,
          { "KDE", "System Settings", "kcm_regionandlang" }, false },
        { ActionId::ChangeLocation, "Change location",
          "Configure languages and regional formats in KDE System Settings.", "preferences-desktop-settings",
          "Clock, Language, and Region", Nav::command({}, kcm("kcm_regionandlang")).target,
          { "KDE", "System Settings", "kcm_regionandlang" }, false },
        { ActionId::ChangeTheDateTimeOrNumberFormat, "Change the date, time, or number format",
          "Configure languages and regional formats in KDE System Settings.", "preferences-desktop-settings",
          "Clock, Language, and Region", Nav::command({}, kcm("kcm_regionandlang")).target,
          { "KDE", "System Settings", "kcm_regionandlang" }, false },
        { ActionId::ChangeKeyboardsOrOtherInputMethods, "Change keyboards or other input methods",
          "Change keyboard repeat, startup Num Lock, layouts and advanced XKB options.", "preferences-desktop-settings",
          "Clock, Language, and Region", Nav::to({}, PageId::Keyboard).target,
          { "KDE", "System Settings", "kcm_keyboard" }, true },
        { ActionId::LetLinuxSuggestSettings, "Let Linux suggest settings",
          "An accessibility recommendation wizard is not implemented.", "preferences-system",
          "Ease of Access", {},
          {  }, false },
        { ActionId::OptimizeVisualDisplay, "Optimize visual display",
          "Open Ease of Access Center.", "preferences-system",
          "Ease of Access", Nav::to({}, PageId::EaseOfAccess).target,
          {  }, false },
        { ActionId::ReplaceSoundsWithVisualCues, "Replace sounds with visual cues",
          "Replace sounds with visual cues is not implemented in this Control Panel.", "preferences-system",
          "Ease of Access", {},
          {  }, false },
        { ActionId::ChangeHowYourMouseWorks, "Change how your mouse works",
          "Change supported mouse and touchpad buttons, pointer motion and scrolling.", "input-mouse",
          "Ease of Access", Nav::to({}, PageId::InputDevices).target,
          { "KDE", "System Settings", "kcm_mouse" }, false },
        { ActionId::ChangeHowYourKeyboardWorks, "Change how your keyboard works",
          "Configure keyboard accessibility features in KDE System Settings.", "preferences-desktop-settings",
          "Ease of Access", Nav::command({}, kcm("kcm_access")).target,
          { "KDE", "System Settings", "kcm_access", "sticky keys", "slow keys", "accessibility" }, true },
        { ActionId::SpeechRecognition, "Speech Recognition",
          "Speech Recognition is not implemented in this Control Panel.", "audio-input-microphone",
          "Ease of Access", {},
          {  }, false },
        { ActionId::StartSpeechRecognition, "Start speech recognition",
          "Start speech recognition is not implemented in this Control Panel.", "preferences-system",
          "Ease of Access", {},
          {  }, false },
        { ActionId::SetUpAMicrophone, "Set up a microphone",
          "Choose and configure a microphone in the native Sound dialog.", "preferences-desktop-settings",
          "Ease of Access", Nav::applet({}, "sound:recording").target,
          { "KDE", "System Settings", "kcm_pulseaudio" }, false },
        { ActionId::ReviewYourComputersStatus, "Review your computer's status",
          "Review the limited security and maintenance status checks performed by this application.", "preferences-system",
          "System and Security", Nav::to({}, PageId::ActionCenter).target,
          {  }, false },
        { ActionId::FindAndFixProblems, "Find and fix problems",
          "A troubleshooting wizard is not implemented.", "preferences-system",
          "System and Security", {},
          {  }, false },
        { ActionId::SetUpParentalControlsForAnyUser, "Set up parental controls for any user",
          "Parental control settings are not implemented.", "preferences-system",
          "User Accounts and Family Safety", {},
          {  }, false },
        { ActionId::ViewDevicesAndPrinters, "View devices and printers",
          "Open Devices and Printers.", "preferences-system",
          "Hardware and Sound", Nav::to({}, PageId::DevicesPrinters).target,
          {  }, false },
        { ActionId::InstalledUpdates, "Installed Updates",
          "Open Installed Updates.", "view-history",
          "Programs", Nav::to({}, PageId::InstalledUpdates).target,
          { "Windows Update history", "package history", "pacman" }, true },
        { ActionId::PerformanceInformationAndTools, "Performance Information and Tools",
          "Open Performance Information and Tools.", "speedometer",
          "System and Security", Nav::to({}, PageId::Performance).target,
          { "Windows Experience Index", "WEI", "benchmark", "processor", "CPU" }, true },
        { ActionId::ChangeDesktopIcons, "Change desktop icons",
          "Inspect and restore the approved Windows 7 Aero icons.", "preferences-desktop-icons",
          "Appearance and Personalization", Nav::to({}, PageId::ThemeComponents).target,
          { "desktop", "icons", "Plasma", "KDE", "System Settings", "kcm_icons" }, true },
        { ActionId::ChangeMousePointers, "Change mouse pointers",
          "Change the size of the approved Aero mouse pointers.", "preferences-desktop-cursors",
          "Appearance and Personalization", Nav::to({}, PageId::Personalization).target,
          { "cursor", "mouse", "pointer", "theme", "KDE", "System Settings", "kcm_cursortheme" }, true },
        { ActionId::ManageAnotherAccount, "Manage another account",
          "Manage user accounts in KDE System Settings.", "system-users",
          "User Accounts and Family Safety", Nav::command({}, kcm("kcm_users")).target,
          { "users", "edit user", "password", "account picture", "administrator", "KDE", "System Settings", "kcm_users" }, true },
        { ActionId::ChangeAdapterSettings, "Change adapter settings",
          "Configure adapters and saved connections with native Windows 7 controls.", "preferences-system-network",
          "Network and Internet", Nav::to({}, PageId::NetworkConnections).target,
          { "network", "connections", "Wi-Fi", "wireless", "Ethernet", "adapter", "KDE", "System Settings", "kcm_networkmanagement" }, true },
        { ActionId::InternetTime, "Internet Time",
          "View time synchronization status and configure the detected time service.", "preferences-system-time",
          "Clock, Language, and Region", Nav::applet({}, "datetime:internet").target,
          { "NTP", "Chrony", "chronyd", "systemd-timesyncd", "timesyncd", "time synchronization", "time server", "clock synchronization" }, false },
        { ActionId::NightTime, "Day and Night",
          "Configure KDE sunrise, sunset, location and custom transition schedules.", "preferences-system-time",
          "Hardware and Sound", Nav::to({}, PageId::NightTime).target,
          { "KDE", "KNightTime", "kcm_nighttime", "sunrise", "sunset", "automatic location", "latitude", "longitude", "schedule", "transition duration" }, true },
        { ActionId::AeroLogin, "Aero Login and Lock Appearance",
          "Review Aero login options and the shared login and lock-screen picture.", "preferences-system-login",
          "Appearance and Personalization", Nav::to({}, PageId::AeroLogin).target,
          { "SDDM", "kcm_aeroshellsddm", "AeroShell", "sign in", "login", "logon", "startup animation", "startup sound", "force user selection", "background", "lock screen picture" }, true },
        { ActionId::NightLight, "Night Light",
          "Adjust KDE Night Light switching mode and day/night color temperatures.", "redshift-status-on",
          "Hardware and Sound", Nav::to({}, PageId::NightLight).target,
          { "KDE", "KWin", "kcm_nightlight", "blue light", "warm", "Kelvin", "temperature", "dark light", "sunrise sunset" }, true },
        { ActionId::WindowBehavior, "Window Behavior",
          "Choose what double-clicking a window title bar does.", "preferences-system-windows",
          "Appearance and Personalization", Nav::to({}, PageId::WindowBehavior).target,
          { "KWin", "title bar", "double click", "maximize", "minimize", "window actions" }, true },
        { ActionId::ChangeWhenTheDisplayTurnsOff, "Change when the display turns off",
          "Configure PowerDevil's idle display timeout.", "preferences-system-power-management",
          "System and Security", Nav::to({}, PageId::PowerOptions).target,
          { "DPMS", "PowerDevil", "screen", "monitor", "idle", "sleep", "timeout" }, false },
        { ActionId::ThemeComponents, "Aero Components",
          "Inspect the approved Aero components and stage Restore Aero defaults.", "preferences-desktop-theme",
          "Appearance and Personalization", Nav::to({}, PageId::ThemeComponents).target,
          { "icons", "cursors", "pointer", "widget style", "application style", "KDE", "Aero" }, true },
        { ActionId::ChangeApplicationStyle, "Aero application style",
          "Inspect the approved Kvantum application style used by Aero.", "preferences-desktop-theme",
          "Appearance and Personalization", Nav::to({}, PageId::ThemeComponents).target,
          { "Qt", "widgets", "Breeze", "Kvantum", "application style" }, false },
        { ActionId::AeroEffects, "Aero Effects",
          "Customize the approved Aero glass effect, including color, transparency and blur.", "preferences-desktop-effects",
          "Appearance and Personalization", Nav::to({}, PageId::AeroEffects).target,
          { "Smod", "Aero Glass", "blur strength", "KWin", "effects", "aeroglassblur" }, true },
        { ActionId::RestoreAeroDefaults, "Restore Aero defaults",
          "Review and stage the approved Aero components and customization defaults in Personalization.", "edit-undo",
          "Appearance and Personalization", Nav::to({}, PageId::Personalization).target,
          { "repair", "reset", "restore", "defaults", "Aero", "theme" }, false },
        { ActionId::NetworkConnections, "Network Connections",
          "View adapters, nearby Wi-Fi and saved connections; configure IP and DNS.", "preferences-system-network",
          "Network and Internet", Nav::to({}, PageId::NetworkConnections).target,
          { "NetworkManager", "adapter", "Ethernet", "Wi-Fi", "wireless", "IPv4", "IPv6", "DNS", "connection", "properties" }, true },
        { ActionId::ManageWirelessNetworks, "Manage wireless networks",
          "View saved network profiles and connect to nearby Wi-Fi networks.", "network-wireless",
          "Network and Internet", Nav::to({}, PageId::NetworkConnections).target,
          { "Wi-Fi", "wireless", "SSID", "saved", "profiles", "password" }, false },
        { ActionId::NetworkConnectionProperties, "Change connection properties",
          "Stage automatic or manual IP addresses and DNS settings for a saved connection.", "preferences-system-network",
          "Network and Internet", Nav::to({}, PageId::NetworkConnections).target,
          { "IP", "IPv4", "IPv6", "DNS", "gateway", "DHCP", "address", "autoconnect", "properties" }, false },
        { ActionId::CustomShortcuts, "Custom Keyboard Shortcuts",
          "Add application shortcuts and create or edit commands with keyboard shortcuts.", "input-keyboard",
          "Hardware and Sound", Nav::to({}, PageId::CustomShortcuts).target,
          { "KDE", "kcm_keys", "custom commands", "application launcher", "script", "command shortcut", "hotkeys" }, true },
        { ActionId::StandardShortcuts, "Application Shortcuts",
          "Change common keyboard shortcuts in KDE applications, including Copy, Open and Save.", "input-keyboard",
          "Hardware and Sound", Nav::to({}, PageId::StandardShortcuts).target,
          { "KDE", "kcm_keys", "KStandardShortcut", "standard shortcuts", "common actions", "copy paste", "application shortcuts" }, true },
        { ActionId::KeyboardShortcuts, "Keyboard Shortcuts",
          "Change global shortcuts, alternative key sequences and component defaults.", "input-keyboard",
          "Hardware and Sound", Nav::to({}, PageId::Shortcuts).target,
          { "KDE", "kcm_keys", "kglobalaccel", "hotkeys", "key bindings", "global shortcuts", "keyboard layout switching", "shortcut conflicts" }, true },
        { ActionId::Keyboard, "Keyboard",
          "Change keyboard repeat, startup Num Lock, layouts and advanced XKB options.", "input-keyboard",
          "Hardware and Sound", Nav::to({}, PageId::Keyboard).target,
          { "KDE", "kcm_keyboard", "XKB", "input languages", "repeat delay", "repeat rate", "Num Lock", "layout switching" }, true },
        { ActionId::Touchpad, "Touchpad",
          "Change supported touchpad tapping, clicking, pointer motion and scrolling.", "input-touchpad",
          "Hardware and Sound", Nav::to({}, PageId::InputDevices).target,
          { "touchpad", "trackpad", "libinput", "tap to click", "natural scrolling", "KWin" }, true },
        { ActionId::ApplicationVolume, "Adjust application volume",
          "Change playback and recording application levels, mute and device routing.", "multimedia-volume-control",
          "Hardware and Sound", Nav::applet({}, "sound:applications").target,
          { "audio mixer", "per application", "routing", "sink input", "source output", "PipeWire", "PulseAudio" }, true },
        { ActionId::RecordingDevices, "Recording devices",
          "Change microphone levels, mute, default recording device and input ports.", "audio-input-microphone",
          "Hardware and Sound", Nav::applet({}, "sound:recording").target,
          { "microphone", "input", "capture", "recording", "audio" }, false },
        { ActionId::PackagePreview, "Ubuntu Package Preview",
          "Simulate an APT full upgrade using cached indexes and inspect proposed package changes.", "system-software-update",
          "Programs", Nav::to({}, PageId::PackagePreview).target,
          { "Ubuntu", "APT", "apt-get", "simulate", "preview", "full upgrade", "dist-upgrade", "PackageKit", "software" }, true },
        { ActionId::ChangeAccountDisplayName, "Change your account display name",
          "Edit the current user's display name through AccountsService with authorization and readback.", "system-users",
          "User Accounts and Family Safety", Nav::to({}, PageId::UserAccounts).target,
          { "AccountsService", "full name", "RealName", "polkit", "account name" }, false },
    };
    return definitions;
}

const ActionDefinition &get(ActionId id)
{
    for (const auto &definition : all())
        if (definition.id == id) return definition;
    static const ActionDefinition missing = {
        ActionId::None, {}, QStringLiteral("This action is not available."), {}, {}, {}, {}, false
    };
    return missing;
}

ActionId idForTarget(const LinkTarget &target)
{
    // Prefer the main page/applet/launcher entry over a task with the same
    // destination, so sidebar aliases still resolve to a canonical action.
    ActionId fallback = ActionId::None;
    for (const auto &definition : all()) {
        if (!sameTarget(definition.target, target)) continue;
        if (definition.iconItem) return definition.id;
        if (fallback == ActionId::None) fallback = definition.id;
    }
    return fallback;
}

Availability availability(const LinkTarget &target)
{
    switch (target.kind) {
    case LinkTarget::None:
        return {false, QStringLiteral("This action is not implemented in this Control Panel.")};
    case LinkTarget::Command:
        if (isAeroPolicyBlockedCommand(target.command)) return {false, aeroPolicyCommandReason()};
        return commandAvailability(target.command);
    case LinkTarget::Callback:
        if (target.callback == CallbackId::CheckUpdates) {
            if (QStandardPaths::findExecutable(QStringLiteral("pacman")).isEmpty())
                return {false, QStringLiteral("Checking for updates requires pacman (Arch-based systems).")};
            if (QStandardPaths::findExecutable(QStringLiteral("checkupdates")).isEmpty())
                return {false, QStringLiteral("Checking for updates requires checkupdates from pacman-contrib.")};
            return {true, {}};
        }
        return {false, QStringLiteral("No callback is configured for this action.")};
    case LinkTarget::Page:
        return target.page == PageId::None
            ? Availability{false, QStringLiteral("No page is configured for this action.")}
            : Availability{true, {}};
    case LinkTarget::Applet:
        if (target.applet == QLatin1String("datetime:additional"))
            return {false, QStringLiteral("Additional clock configuration is not implemented in this Control Panel.")};
        if (target.applet == QLatin1String("sound:sounds")
            || target.applet == QLatin1String("sound:communications"))
            return {false, QStringLiteral("Sound event and communication configuration is not implemented in this Control Panel.")};
        if (target.applet == QLatin1String("datetime")
            || target.applet == QLatin1String("datetime:internet")
            || target.applet == QLatin1String("sound")
            || target.applet == QLatin1String("sound:applications")
            || target.applet == QLatin1String("sound:recording"))
            return {true, {}};
        return {false, QStringLiteral("This applet is not available.")};
    case LinkTarget::Home:
        return {true, {}};
    }
    return {false, QStringLiteral("This action is not available.")};
}

Availability availability(ActionId id)
{
    const auto &definition = get(id);
    if (definition.target.kind == LinkTarget::None)
        return {false, definition.description};
    return availability(definition.target);
}

QList<ActionId> search(const QString &query)
{
    const QString cleaned = normalized(query);
    const QStringList tokens = cleaned.split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts);
    if (tokens.isEmpty()) return {};
    struct Match { ActionId id; int titleRank; bool available; QString title; };
    QList<Match> matches;
    for (const auto &definition : all()) {
        const QString title = normalized(definition.title);
        const QString haystack = title + QLatin1Char(' ') + normalized(definition.description)
            + QLatin1Char(' ') + normalized(definition.category)
            + QLatin1Char(' ') + normalized(definition.keywords.join(QLatin1Char(' ')));
        bool matched = true;
        int rank = title == cleaned ? 1000 : title.startsWith(cleaned) ? 600
                                   : title.contains(cleaned) ? 400 : 0;
        for (const QString &keyword : definition.keywords)
            if (normalized(keyword) == cleaned) { rank += 20; break; }
        for (const QString &token : tokens) {
            if (!haystack.contains(token)) { matched = false; break; }
            if (title.contains(token)) rank += 10;
        }
        if (matched)
            matches.append({definition.id, rank, availability(definition.id).available, title});
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        if (a.titleRank != b.titleRank) return a.titleRank > b.titleRank;
        if (a.available != b.available) return a.available;
        return a.title < b.title;
    });
    QList<ActionId> result;
    for (const auto &match : matches) result.append(match.id);
    return result;
}
} // namespace ActionCatalog
