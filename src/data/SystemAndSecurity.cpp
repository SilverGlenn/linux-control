#include "SystemAndSecurity.h"

const QList<DetailGroup> &systemAndSecurityGroups()
{
    static const QList<DetailGroup> groups = {
        {
            ActionId::ActionCenter,
            {
                { ActionId::ReviewYourComputersStatusAndResolveIssues },
                { ActionId::ChangeUserAccountControlSettings,
                  ActionId::TroubleshootCommonComputerProblems },
                { ActionId::RestoreYourComputerToAnEarlierTime },
            }
        },
        {
            ActionId::LinuxFirewall,
            {
                { ActionId::CheckFirewallStatus,
                  ActionId::AllowAProgramThroughLinuxFirewall },
            }
        },
        {
            ActionId::System,
            {
                { ActionId::ViewAmountOfRAMAndProcessorSpeed,
                  ActionId::CheckTheLinuxExperienceIndex },
                { ActionId::AllowRemoteAccess,
                  ActionId::SeeTheNameOfThisComputer,
                  ActionId::DeviceManager },
            }
        },
        {
            ActionId::LinuxUpdate,
            {
                { ActionId::TurnAutomaticUpdatingOnOrOff,
                  ActionId::CheckForUpdates,
                  ActionId::ViewInstalledUpdates },
            }
        },
        {
            ActionId::PowerOptions,
            {
                { ActionId::RequireAPasswordWhenTheComputerWakes,
                  ActionId::ChangeWhatThePowerButtonsDo },
                { ActionId::ChangeWhenTheComputerSleeps, ActionId::ChangeWhenTheDisplayTurnsOff },
            }
        },
        {
            ActionId::BackupAndRestore,
            {
                { ActionId::BackUpYourComputer,
                  ActionId::RestoreFilesFromBackup },
            }
        },
        {
            ActionId::BitLockerDriveEncryption,
            {
                { ActionId::ProtectYourComputerByEncryptingDataOnYourDisk,
                  ActionId::ManageBitLocker },
            }
        },
        {
            ActionId::AdministrativeTools,
            {
                { ActionId::FreeUpDiskSpace,
                  ActionId::DefragmentYourHardDrive },
                { ActionId::CreateAndFormatHardDiskPartitions,
                  ActionId::ViewEventLogs,
                  ActionId::ScheduleTasks },
            }
        },
    };
    return groups;
}
