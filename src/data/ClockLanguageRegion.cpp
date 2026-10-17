#include "ClockLanguageRegion.h"

const QList<DetailGroup> &clockLanguageRegionGroups()
{
    static const QList<DetailGroup> groups = {
        {
            ActionId::DateAndTime,
            {
                { ActionId::SetTheTimeAndDate,
                  ActionId::ChangeTheTimeZone,
                  ActionId::AddClocksForDifferentTimeZones },
                { ActionId::AddTheClockGadgetToTheDesktop },
            }
        },
        {
            ActionId::RegionAndLanguage,
            {
                { ActionId::InstallOrUninstallDisplayLanguages,
                  ActionId::ChangeDisplayLanguage,
                  ActionId::ChangeLocation },
                { ActionId::ChangeTheDateTimeOrNumberFormat,
                  ActionId::ChangeKeyboardsOrOtherInputMethods },
            }
        },
    };
    return groups;
}
