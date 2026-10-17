#include <QtTest>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>

#include "ActionCatalog.h"
#include "Categories.h"
#include "Commands.h"

namespace {
class ScopedPath {
public:
    explicit ScopedPath(const QByteArray &path)
        : m_previous(qgetenv("PATH")), m_wasSet(qEnvironmentVariableIsSet("PATH"))
    { qputenv("PATH", path); }
    ~ScopedPath()
    {
        if (m_wasSet) qputenv("PATH", m_previous);
        else qunsetenv("PATH");
    }
private:
    QByteArray m_previous;
    bool m_wasSet;
};
}

class ActionCatalogTest : public QObject {
    Q_OBJECT
private slots:
    void stableIdentityAndCategoryIntegrity()
    {
        QSet<int> ids;
        QSet<QString> iconTitles;
        for (const auto &action : ActionCatalog::all()) {
            QVERIFY(action.id != ActionId::None);
            QVERIFY(!ids.contains(int(action.id)));
            ids.insert(int(action.id));
            QCOMPARE(ActionCatalog::get(action.id).id, action.id);
            QVERIFY(!action.title.isEmpty());
            QVERIFY(!action.description.isEmpty());
            QVERIFY(navOrder().contains(action.category));
            if (action.iconItem) {
                const QString title = action.title.toCaseFolded();
                QVERIFY(!iconTitles.contains(title));
                iconTitles.insert(title);
            }
            const auto state = ActionCatalog::availability(action.id);
            QVERIFY(state.available || !state.reason.isEmpty());
        }
        for (const auto &category : homeCategories())
            for (ActionId id : category.tasks)
                QVERIFY(ids.contains(int(id)));
        for (const auto &category : navOrder()) {
            const auto *groups = detailGroupsFor(category);
            QVERIFY(groups);
            for (const auto &group : *groups) {
                QVERIFY(ids.contains(int(group.action)));
                for (const auto &line : group.lines)
                    for (ActionId id : line) QVERIFY(ids.contains(int(id)));
            }
        }
        QCOMPARE(ActionCatalog::get(ActionId(-1)).id, ActionId::None);
        QVERIFY(!ActionCatalog::availability(ActionId::None).available);
    }

    void nativePagesAndDialogsAreDiscoverable()
    {
        const QList<PageId> pages = {
            PageId::LinuxUpdate, PageId::ProgramsFeatures, PageId::InstalledUpdates,
            PageId::NetworkSharing, PageId::Firewall, PageId::ActionCenter,
            PageId::PowerOptions, PageId::Performance, PageId::Personalization,
            PageId::Fonts, PageId::UserAccounts, PageId::EaseOfAccess,
            PageId::DevicesPrinters, PageId::System, PageId::NightLight, PageId::NightTime, PageId::ScreenLock, PageId::AeroLogin, PageId::Display, PageId::WindowBehavior,
            PageId::DesktopBackground, PageId::ThemeComponents, PageId::FontSettings,
            PageId::AeroEffects, PageId::PackagePreview, PageId::NetworkConnections, PageId::InputDevices, PageId::Keyboard, PageId::DefaultApps, PageId::Shortcuts, PageId::StandardShortcuts, PageId::CustomShortcuts
        };
        for (PageId page : pages) {
            const ActionId id = ActionCatalog::idForTarget(Nav::to({}, page).target);
            QVERIFY(id != ActionId::None);
            QVERIFY(ActionCatalog::get(id).iconItem);
            QVERIFY(ActionCatalog::availability(id).available);
        }
        for (const auto &name : {QStringLiteral("datetime"), QStringLiteral("sound")}) {
            const ActionId id = ActionCatalog::idForTarget(Nav::applet({}, name).target);
            QVERIFY(ActionCatalog::get(id).iconItem);
            QVERIFY(ActionCatalog::availability(id).available);
        }
        QCOMPARE(ActionCatalog::get(ActionId::ViewDevicesAndPrinters).target.page,
                 PageId::DevicesPrinters);
        QCOMPARE(ActionCatalog::get(ActionId::CheckForUpdates).target.kind,
                 LinkTarget::Callback);
        QCOMPARE(ActionCatalog::get(ActionId::CheckForUpdates).target.callback,
                 CallbackId::CheckUpdates);
    }

    void normalizedSearchRequiresEveryToken()
    {
        const auto system = ActionCatalog::search(QString::fromUtf8("  SÝSTÈM  "));
        QVERIFY(!system.isEmpty());
        QCOMPARE(system.first(), ActionId::System);
        QVERIFY(ActionCatalog::search("system impossible-token").isEmpty());
        QVERIFY(ActionCatalog::search("   ").isEmpty());
        QVERIFY(ActionCatalog::search("hardware USB").contains(ActionId::DevicesAndPrinters));
        QVERIFY(ActionCatalog::search("WINDOWS firewall").contains(ActionId::LinuxFirewall));
        QVERIFY(ActionCatalog::search("KDE cursor").contains(ActionId::ChangeMousePointers));
        QCOMPARE(ActionCatalog::search("Windows Update").first(), ActionId::LinuxUpdate);
    }

    void appearanceTasksUseNativeEditors()
    {
        QCOMPARE(ActionCatalog::get(ActionId::ChangeDesktopBackground).target.page, PageId::DesktopBackground);
        QCOMPARE(ActionCatalog::get(ActionId::ChangeDesktopIcons).target.page, PageId::ThemeComponents);
        QCOMPARE(ActionCatalog::get(ActionId::ChangeMousePointers).target.page, PageId::Personalization);
        QCOMPARE(ActionCatalog::get(ActionId::ChangeApplicationStyle).target.page, PageId::ThemeComponents);
        QCOMPARE(ActionCatalog::get(ActionId::ChangeFontSettings).target.page, PageId::FontSettings);
        QCOMPARE(ActionCatalog::get(ActionId::AdjustClearTypeText).target.page, PageId::FontSettings);
        QVERIFY(ActionCatalog::search("subpixel").contains(ActionId::ChangeFontSettings));
        QVERIFY(ActionCatalog::search("widget style").contains(ActionId::ThemeComponents));
        QCOMPARE(ActionCatalog::get(ActionId::AeroEffects).target.page, PageId::AeroEffects);
        QCOMPARE(ActionCatalog::get(ActionId::PackagePreview).target.page, PageId::PackagePreview);
        QCOMPARE(ActionCatalog::get(ActionId::ChangeAccountDisplayName).target.page, PageId::UserAccounts);
        QVERIFY(ActionCatalog::search("APT simulate").contains(ActionId::PackagePreview));
        QVERIFY(ActionCatalog::search("Smod blur").contains(ActionId::AeroEffects));
    }

    void nativeDisplayAndPowerRoutesAndSearch()
    {
        for (const auto id : {ActionId::Display, ActionId::AdjustScreenResolution,
                ActionId::MakeTextAndOtherItemsLargerOrSmaller, ActionId::ConnectToAnExternalDisplay}) {
            QCOMPARE(ActionCatalog::get(id).target.kind, LinkTarget::Page);
            QCOMPARE(ActionCatalog::get(id).target.page, PageId::Display);
        }
        QCOMPARE(ActionCatalog::get(ActionId::ChangeWhatThePowerButtonsDo).target.page, PageId::PowerOptions);
        QVERIFY(ActionCatalog::availability(ActionId::ChangeWhatThePowerButtonsDo).available);
        QVERIFY(ActionCatalog::search("display orientation").contains(ActionId::Display));
        QVERIFY(ActionCatalog::search("critical battery").contains(ActionId::PowerOptions));
    }

    void nativeScreenLockRouteAndSearch()
    {
        QCOMPARE(ActionCatalog::get(ActionId::ChangeScreenLockSettings).target.page, PageId::ScreenLock);
        QCOMPARE(ActionCatalog::get(ActionId::RequireAPasswordWhenTheComputerWakes).target.page, PageId::ScreenLock);
        QVERIFY(ActionCatalog::availability(ActionId::ChangeScreenLockSettings).available);
        for (const auto *query : {"idle timeout", "password grace", "kcm_screenlocker", "lock screen"})
            QVERIFY2(ActionCatalog::search(QString::fromLatin1(query)).contains(ActionId::ChangeScreenLockSettings), query);
    }
    void nativeAeroLoginRouteAndSearch()
    {
        QCOMPARE(ActionCatalog::get(ActionId::AeroLogin).target.page, PageId::AeroLogin);
        QVERIFY(ActionCatalog::availability(ActionId::AeroLogin).available);
        for (const char *query : {"SDDM", "kcm_aeroshellsddm", "startup animation", "logon", "lock screen picture"})
            QVERIFY2(ActionCatalog::search(QString::fromLatin1(query)).contains(ActionId::AeroLogin), query);
    }

    void nativeNightTimeRouteAndSearch()
    {
        QCOMPARE(ActionCatalog::get(ActionId::NightTime).target.kind, LinkTarget::Page);
        QCOMPARE(ActionCatalog::get(ActionId::NightTime).target.page, PageId::NightTime);
        QVERIFY(ActionCatalog::availability(ActionId::NightTime).available);
        for (const auto *query : {"sunrise", "sunset", "latitude", "kcm_nighttime", "transition duration"})
            QVERIFY2(ActionCatalog::search(QString::fromLatin1(query)).contains(ActionId::NightTime), query);
    }

    void nativeNightLightRouteAndSearch()
    {
        QCOMPARE(ActionCatalog::get(ActionId::NightLight).target.kind, LinkTarget::Page);
        QCOMPARE(ActionCatalog::get(ActionId::NightLight).target.page, PageId::NightLight);
        QVERIFY(ActionCatalog::availability(ActionId::NightLight).available);
        for (const auto *query : {"blue light", "Kelvin", "night light", "kcm_nightlight"})
            QVERIFY2(ActionCatalog::search(QString::fromLatin1(query)).contains(ActionId::NightLight), query);
    }

    void aeroPolicyCoversIndirectLaunches()
    {
        for (const QString &module : {QStringLiteral("kcm_colors"), QStringLiteral("kcm_icons"),
                QStringLiteral("kcm_cursortheme"), QStringLiteral("kcm_lookandfeel"),
                QStringLiteral("kcm_desktoptheme"), QStringLiteral("kcm_kwindecoration"),
                QStringLiteral("kcmstyle"), QStringLiteral("kcm_style"), QStringLiteral("kcm_kwin_effects")}) {
            const auto target = Nav::command({}, {QStringLiteral("/usr/bin/kcmshell6"), module + QStringLiteral(".desktop")}).target;
            QVERIFY(isAeroPolicyBlockedCommand(target.command));
            const auto availability = ActionCatalog::availability(target);
            QVERIFY(!availability.available);
            QVERIFY(availability.reason.contains(QStringLiteral("Aero")));
        }
        QVERIFY(isAeroPolicyBlockedCommand({QStringLiteral("plasma-apply-colorscheme"), QStringLiteral("BreezeDark")}));
        QVERIFY(isAeroPolicyBlockedCommand({QStringLiteral("lookandfeeltool"), QStringLiteral("-a"), QStringLiteral("other")}));
        QVERIFY(!isAeroPolicyBlockedCommand(kcm("kcm_access")));
        for (const auto &action : ActionCatalog::all())
            if (action.target.kind == LinkTarget::Command)
                QVERIFY2(!isAeroPolicyBlockedCommand(action.target.command), qPrintable(action.title));
        QCOMPARE(ActionCatalog::get(ActionId::RestoreAeroDefaults).target.page, PageId::Personalization);
        QVERIFY(ActionCatalog::search("restore Aero defaults").contains(ActionId::RestoreAeroDefaults));
    }

    void nativeNetworkingRoutesAndSearch()
    {
        for (ActionId id : {ActionId::NetworkConnections, ActionId::ChangeAdapterSettings,
                ActionId::ConnectToANetwork, ActionId::ManageWirelessNetworks, ActionId::NetworkConnectionProperties}) {
            QCOMPARE(ActionCatalog::get(id).target.kind, LinkTarget::Page);
            QCOMPARE(ActionCatalog::get(id).target.page, PageId::NetworkConnections);
        }
        QVERIFY(ActionCatalog::search("IPv4 DNS").contains(ActionId::NetworkConnectionProperties));
        QVERIFY(ActionCatalog::search("saved Wi-Fi").contains(ActionId::ManageWirelessNetworks));
        QVERIFY(ActionCatalog::search("NetworkManager").contains(ActionId::NetworkConnections));
    }

    void nativeAudioRoutesAndSearch()
    {
        QCOMPARE(ActionCatalog::get(ActionId::AdjustSystemVolume).target.kind, LinkTarget::Applet);
        QCOMPARE(ActionCatalog::get(ActionId::AdjustSystemVolume).target.applet, QStringLiteral("sound"));
        QCOMPARE(ActionCatalog::get(ActionId::ApplicationVolume).target.applet, QStringLiteral("sound:applications"));
        QCOMPARE(ActionCatalog::get(ActionId::RecordingDevices).target.applet, QStringLiteral("sound:recording"));
        QVERIFY(ActionCatalog::search("audio mixer").contains(ActionId::ApplicationVolume));
        QVERIFY(ActionCatalog::search("microphone").contains(ActionId::RecordingDevices));
        QVERIFY(ActionCatalog::availability(ActionId::ApplicationVolume).available);
    }

    void nativePointingDeviceRoutesAndSearch()
    {
        for (const auto id : {ActionId::Mouse, ActionId::Touchpad, ActionId::ChangeHowYourMouseWorks}) {
            QCOMPARE(ActionCatalog::get(id).target.kind, LinkTarget::Page);
            QCOMPARE(ActionCatalog::get(id).target.page, PageId::InputDevices);
        }
        QVERIFY(ActionCatalog::search("trackpad").contains(ActionId::Touchpad));
        QVERIFY(ActionCatalog::search("pointer buttons").contains(ActionId::Mouse));
    }

    void keyboardAndDefaultProgramsUseNativePages()
    {
        for (auto id : {ActionId::Keyboard, ActionId::ChangeKeyboardsOrOtherInputMethods}) {
            QCOMPARE(ActionCatalog::get(id).target.kind, LinkTarget::Page);
            QCOMPARE(ActionCatalog::get(id).target.page, PageId::Keyboard);
        }
        for (auto id : {ActionId::DefaultPrograms, ActionId::SetYourDefaultPrograms,
                       ActionId::MakeAFileTypeAlwaysOpenInASpecificProgram}) {
            QCOMPARE(ActionCatalog::get(id).target.kind, LinkTarget::Page);
            QCOMPARE(ActionCatalog::get(id).target.page, PageId::DefaultApps);
        }
        QVERIFY(ActionCatalog::search("XKB repeat delay").contains(ActionId::Keyboard));
        QVERIFY(ActionCatalog::search("file associations").contains(ActionId::DefaultPrograms));
        QCOMPARE(ActionCatalog::get(ActionId::ChangeHowYourKeyboardWorks).target.kind, LinkTarget::Command);
        QCOMPARE(ActionCatalog::get(ActionId::SetUpAMicrophone).target.applet, QStringLiteral("sound:recording"));
    }

    void nativeShortcutsRouteAndSearch()
    {
        QCOMPARE(ActionCatalog::get(ActionId::KeyboardShortcuts).target.kind, LinkTarget::Page);
        QCOMPARE(ActionCatalog::get(ActionId::KeyboardShortcuts).target.page, PageId::Shortcuts);
        QVERIFY(ActionCatalog::search("global shortcuts").contains(ActionId::KeyboardShortcuts));
        QVERIFY(ActionCatalog::search("hotkeys").contains(ActionId::KeyboardShortcuts));
        QVERIFY(ActionCatalog::search("kcm_keys").contains(ActionId::KeyboardShortcuts));
        QCOMPARE(ActionCatalog::get(ActionId::StandardShortcuts).target.kind, LinkTarget::Page);
        QCOMPARE(ActionCatalog::get(ActionId::StandardShortcuts).target.page, PageId::StandardShortcuts);
        QVERIFY(ActionCatalog::search("standard shortcuts").contains(ActionId::StandardShortcuts));
        QVERIFY(ActionCatalog::search("copy paste").contains(ActionId::StandardShortcuts));
        QCOMPARE(ActionCatalog::get(ActionId::CustomShortcuts).target.page, PageId::CustomShortcuts);
        QVERIFY(ActionCatalog::search("custom commands").contains(ActionId::CustomShortcuts));
    }

    void timeSynchronizationAliasesFindTheActualTab()
    {
        for (const QString &query : {QStringLiteral("NTP"), QStringLiteral("Chrony"),
                                     QStringLiteral("timesyncd"), QStringLiteral("time synchronization")})
            QVERIFY2(ActionCatalog::search(query).contains(ActionId::InternetTime), qPrintable(query));
        QCOMPARE(ActionCatalog::search("Internet Time").first(), ActionId::InternetTime);
        const auto &internet = ActionCatalog::get(ActionId::InternetTime);
        QCOMPARE(internet.target.kind, LinkTarget::Applet);
        QCOMPARE(internet.target.applet, QStringLiteral("datetime:internet"));
        QVERIFY(ActionCatalog::availability(internet.id).available);
        QVERIFY(ActionCatalog::search("NTP Chrony").contains(ActionId::DateAndTime));
    }

    void unsupportedActionsStayVisibleWithReasons()
    {
        const auto &backup = ActionCatalog::get(ActionId::BackUpYourComputer);
        QCOMPARE(backup.target.kind, LinkTarget::None);
        QVERIFY(ActionCatalog::search("Back up your computer").contains(backup.id));
        const auto state = ActionCatalog::availability(backup.id);
        QVERIFY(!state.available);
        QVERIFY(!state.reason.isEmpty());
        QCOMPARE(ActionCatalog::get(ActionId::ChooseHomegroupAndSharingOptions).target.kind,
                 LinkTarget::None);
        QCOMPARE(ActionCatalog::get(ActionId::ChangeWhenTheComputerSleeps).target.page,
                 PageId::PowerOptions);
    }

    void unimplementedDialogControlsStayDisabled()
    {
        for (ActionId id : {ActionId::ChangeSystemSounds, ActionId::ChangeSoundEffects,
                            ActionId::AddClocksForDifferentTimeZones}) {
            QCOMPARE(ActionCatalog::get(id).target.kind, LinkTarget::None);
            QVERIFY(!ActionCatalog::availability(id).available);
            QVERIFY(!ActionCatalog::availability(id).reason.isEmpty());
            QVERIFY(ActionCatalog::search(ActionCatalog::get(id).title).contains(id));
        }
        const auto &volume = ActionCatalog::get(ActionId::AdjustSystemVolume);
        QCOMPARE(volume.target.kind, LinkTarget::Applet);
        QCOMPARE(volume.target.applet, QStringLiteral("sound"));
        QVERIFY(volume.target.command.isEmpty());
    }

    void availabilityOnlyInspectsLaunchers()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ScopedPath path(dir.path().toUtf8());
        const auto missing = ActionCatalog::availability(
            Nav::command({}, {QStringLiteral("catalog-test-launcher")}).target);
        QVERIFY(!missing.available);
        QVERIFY(missing.reason.contains("catalog-test-launcher"));
        QFile launcher(dir.filePath("catalog-test-launcher"));
        QVERIFY(launcher.open(QIODevice::WriteOnly));
        launcher.write("#!/bin/sh\nexit 99\n");
        launcher.close();
        QVERIFY(launcher.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner));
        const auto installed = ActionCatalog::availability(
            Nav::command({}, {QStringLiteral("catalog-test-launcher")}).target);
        QVERIFY(installed.available);
        QVERIFY(installed.reason.isEmpty());
        // No pacman/checkupdates in this isolated path: the native update page
        // remains readable, while the explicit check callback is unavailable.
        QVERIFY(ActionCatalog::availability(ActionId::LinuxUpdate).available);
        QVERIFY(!ActionCatalog::availability(ActionId::CheckForUpdates).available);
        QVERIFY(ActionCatalog::availability(ActionId::Mouse).available);
    }
};

QTEST_GUILESS_MAIN(ActionCatalogTest)
#include "action_catalog_test.moc"
