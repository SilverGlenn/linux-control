#include "AppearanceAndPersonalization.h"

const QList<DetailGroup> &appearanceAndPersonalizationGroups()
{
    static const QList<DetailGroup> groups = {
        {
            ActionId::Personalization,
            {
                { ActionId::ChangeWindowColors,
                  ActionId::ChangeDesktopBackground,
                  ActionId::ChangeWindowGlassColors },
                { ActionId::RestoreAeroDefaults },
                { ActionId::ChangeSoundEffects,
                  ActionId::ChangeScreenLockSettings, ActionId::AeroLogin },
            }
        },
        {
            ActionId::Display,
            {
                { ActionId::MakeTextAndOtherItemsLargerOrSmaller,
                  ActionId::AdjustScreenResolution },
                { ActionId::ConnectToAnExternalDisplay, ActionId::NightLight },
            }
        },
        { ActionId::WindowBehavior, {} },
        { ActionId::AeroEffects, {} },
        { ActionId::ThemeComponents, {{ ActionId::ChangeDesktopIcons, ActionId::ChangeMousePointers,
                                        ActionId::ChangeApplicationStyle }} },
        {
            ActionId::DesktopGadgets,
            {
                { ActionId::AddGadgetsToTheDesktop,
                  ActionId::GetMoreGadgetsOnline,
                  ActionId::UninstallAGadget },
                { ActionId::RestoreDesktopGadgetsInstalledWithLinux },
            }
        },
        {
            ActionId::TaskbarAndStartMenu,
            {
                { ActionId::CustomizeTheStartMenu,
                  ActionId::CustomizeIconsOnTheTaskbar },
                { ActionId::ChangeThePictureOnTheStartMenu },
            }
        },
        {
            ActionId::EaseOfAccessCenter,
            {
                { ActionId::AccommodateLowVision,
                  ActionId::UseScreenReader,
                  ActionId::TurnOnEasyAccessKeys },
                { ActionId::TurnHighContrastOnOrOff },
            }
        },
        {
            ActionId::FolderOptions,
            {
                { ActionId::SpecifySingleOrDoubleClickToOpen,
                  ActionId::ShowHiddenFilesAndFolders },
            }
        },
        {
            ActionId::Fonts,
            {
                { ActionId::PreviewInstalledFonts,
                  ActionId::ChangeFontSettings,
                  ActionId::AdjustClearTypeText },
            }
        },
    };
    return groups;
}
