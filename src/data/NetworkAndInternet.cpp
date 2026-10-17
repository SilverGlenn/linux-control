#include "NetworkAndInternet.h"

const QList<DetailGroup> &networkAndInternetGroups()
{
    static const QList<DetailGroup> groups = {
        {
            ActionId::NetworkAndSharingCenter,
            {
                { ActionId::ViewNetworkStatusAndTasks,
                  ActionId::ConnectToANetwork },
                { ActionId::ViewNetworkComputersAndDevices,
                  ActionId::AddAWirelessDeviceToTheNetwork },
            }
        },
        { ActionId::NetworkConnections, {{ ActionId::ChangeAdapterSettings, ActionId::ManageWirelessNetworks,
                                            ActionId::NetworkConnectionProperties }} },
        {
            ActionId::HomeGroup,
            {
                { ActionId::ChooseHomegroupAndSharingOptions },
            }
        },
        {
            ActionId::InternetOptions,
            {
                { ActionId::ChangeYourHomepage,
                  ActionId::ManageBrowserAddOns,
                  ActionId::DeleteBrowsingHistoryAndCookies },
            }
        },
    };
    return groups;
}
