#include "Categories.h"

#include "SystemAndSecurity.h"
#include "NetworkAndInternet.h"
#include "HardwareAndSound.h"
#include "Programs.h"
#include "UserAccounts.h"
#include "AppearanceAndPersonalization.h"
#include "ClockLanguageRegion.h"
#include "EaseOfAccess.h"

const QList<CategoryItem> &homeCategories()
{
    static const QList<CategoryItem> items = {
        {
            "dialog-password",
            "System and Security",
            { ActionId::ReviewYourComputersStatus, ActionId::BackUpYourComputer, ActionId::FindAndFixProblems }
        },
        {
            "system-users",
            "User Accounts and Family Safety",
            { ActionId::AddOrRemoveUserAccounts, ActionId::SetUpParentalControlsForAnyUser }
        },
        {
            "folder-network",
            "Network and Internet",
            { ActionId::ViewNetworkStatusAndTasks, ActionId::ChooseHomegroupAndSharingOptions }
        },
        {
            "preferences-desktop-theme-global",
            "Appearance and Personalization",
            { ActionId::ChangeWindowColors, ActionId::ChangeDesktopBackground, ActionId::AdjustScreenResolution }
        },
        {
            "input_devices_settings",
            "Hardware and Sound",
            { ActionId::ViewDevicesAndPrinters, ActionId::AddADevice }
        },
        {
            "preferences-desktop-locale",
            "Clock, Language, and Region",
            { ActionId::ChangeKeyboardsOrOtherInputMethods, ActionId::ChangeDisplayLanguage }
        },
        {
            "application-vnd.debian.binary-package",
            "Programs",
            { ActionId::UninstallAProgram }
        },
        {
            "preferences-desktop-accessibility",
            "Ease of Access",
            { ActionId::LetLinuxSuggestSettings, ActionId::OptimizeVisualDisplay }
        },
    };
    return items;
}

const QStringList &navOrder()
{
    static const QStringList order = {
        "System and Security",
        "Network and Internet",
        "Hardware and Sound",
        "Programs",
        "User Accounts and Family Safety",
        "Appearance and Personalization",
        "Clock, Language, and Region",
        "Ease of Access",
    };
    return order;
}

const QList<DetailGroup> *detailGroupsFor(const QString &category)
{
    if (category == "System and Security")
        return &systemAndSecurityGroups();
    if (category == "Network and Internet")
        return &networkAndInternetGroups();
    if (category == "Hardware and Sound")
        return &hardwareAndSoundGroups();
    if (category == "Programs")
        return &programsGroups();
    if (category == "User Accounts and Family Safety")
        return &userAccountsGroups();
    if (category == "Appearance and Personalization")
        return &appearanceAndPersonalizationGroups();
    if (category == "Clock, Language, and Region")
        return &clockLanguageRegionGroups();
    if (category == "Ease of Access")
        return &easeOfAccessGroups();
    return nullptr;
}
