#include "UserAccounts.h"

const QList<DetailGroup> &userAccountsGroups()
{
    static const QList<DetailGroup> groups = {
        {
            ActionId::UserAccounts,
            {
                { ActionId::ChangeYourAccountPicture,
                  ActionId::AddOrRemoveUserAccounts },
                { ActionId::ChangeYourLinuxPassword },
            }
        },
        {
            ActionId::CredentialManager,
            {
                { ActionId::ManageLinuxCredentials },
            }
        },
    };
    return groups;
}
