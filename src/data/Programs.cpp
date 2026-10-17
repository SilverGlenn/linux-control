#include "Programs.h"

const QList<DetailGroup> &programsGroups()
{
    static const QList<DetailGroup> groups = {
        {
            ActionId::ProgramsAndFeatures,
            {
                { ActionId::UninstallAProgram,
                  ActionId::TurnLinuxFeaturesOnOrOff,
                  ActionId::ViewInstalledUpdates },
                { ActionId::RunProgramsMadeForPreviousVersionsOfLinux,
                  ActionId::HowToInstallAProgram },
            }
        },
        {
            ActionId::DefaultPrograms,
            {
                { ActionId::ChangeDefaultSettingsForMediaOrDevices },
                { ActionId::MakeAFileTypeAlwaysOpenInASpecificProgram,
                  ActionId::SetYourDefaultPrograms },
            }
        },
        {
            ActionId::DesktopGadgets,
            {
                { ActionId::AddGadgetsToTheDesktop,
                  ActionId::GetMoreGadgetsOnline,
                  ActionId::UninstallAGadget },
                { ActionId::RestoreDesktopGadgetsInstalledWithLinux },
            }
        },
    };
    return groups;
}
