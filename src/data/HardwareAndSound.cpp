#include "HardwareAndSound.h"

const QList<DetailGroup> &hardwareAndSoundGroups()
{
    static const QList<DetailGroup> groups = {
        {
            ActionId::DevicesAndPrinters,
            {
                { ActionId::AddADevice,
                  ActionId::AddAPrinter,
                  ActionId::Mouse,
                  ActionId::Keyboard,
                  ActionId::DeviceManager },
                { ActionId::Touchpad, ActionId::KeyboardShortcuts, ActionId::StandardShortcuts, ActionId::CustomShortcuts },
            }
        },
        {
            ActionId::AutoPlay,
            {
                { ActionId::ChangeDefaultSettingsForMediaOrDevices,
                  ActionId::PlayCDsOrOtherMediaAutomatically },
            }
        },
        {
            ActionId::Sound,
            {
                { ActionId::AdjustSystemVolume,
                  ActionId::ChangeSystemSounds,
                  ActionId::ManageAudioDevices },
                { ActionId::ApplicationVolume, ActionId::RecordingDevices },
            }
        },
        {
            ActionId::PowerOptions,
            {
                { ActionId::ChangePowerSavingSettings,
                  ActionId::ChangeWhatThePowerButtonsDo },
                { ActionId::RequireAPasswordWhenTheComputerWakes,
                  ActionId::ChangeWhenTheComputerSleeps },
                { ActionId::ChooseAPowerPlan },
            }
        },
        {
            ActionId::Display,
            {
                { ActionId::MakeTextAndOtherItemsLargerOrSmaller,
                  ActionId::AdjustScreenResolution },
                { ActionId::NightLight, ActionId::NightTime },
                { ActionId::ConnectToAnExternalDisplay,
                  ActionId::HowToCorrectMonitorFlickerRefreshRate },
            }
        },
    };
    return groups;
}
