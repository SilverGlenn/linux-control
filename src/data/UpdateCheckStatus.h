#pragma once

// Repository update checks are required for a complete package status. AUR
// checks are optional, but their failure must not erase known repository
// updates or turn an incomplete result into a full "up to date" claim.
enum class PackageUpdateCheckState {
    RepositoryCheckFailed,
    FullyUpToDate,
    UpdatesAvailable,
    UpdatesAvailableAurUnknown,
    NoKnownUpdatesAurUnknown,
};

inline PackageUpdateCheckState classifyPackageUpdateCheck(
    bool repositoryCheckSucceeded, int repositoryUpdates,
    bool aurCheckSucceeded, int aurUpdates)
{
    if (!repositoryCheckSucceeded)
        return PackageUpdateCheckState::RepositoryCheckFailed;

    const bool updatesKnown = repositoryUpdates > 0 || aurUpdates > 0;
    if (!aurCheckSucceeded)
        return updatesKnown ? PackageUpdateCheckState::UpdatesAvailableAurUnknown
                            : PackageUpdateCheckState::NoKnownUpdatesAurUnknown;
    return updatesKnown ? PackageUpdateCheckState::UpdatesAvailable
                        : PackageUpdateCheckState::FullyUpToDate;
}
