#pragma once
#include "settings/AeroThemeContract.h"
#include "settings/AeroThemeSettings.h"

// An isolated in-memory desktop for navigation and view integration tests.
class FixtureAeroThemeBackend : public AeroThemeBackend {
public:
    AeroThemeState current;
    int writes = 0;
    bool refuseWrite = false;
    FixtureAeroThemeBackend()
    {
        current.values = AeroThemeContract::defaults();
        current.assetsSupported = current.runtimeSupported = true;
        for (auto it = current.values.cbegin(); it != current.values.cend(); ++it)
            current.writableKeys.insert(it.key());
    }
    void readCurrent() override { emit readFinished(true, current, {}); }
    void writeSettings(const QVariantMap &, const QVariantMap &requested) override
    {
        ++writes;
        if (refuseWrite) { emit writeFinished(false, QStringLiteral("Test refusal")); return; }
        current.values = requested;
        emit writeFinished(true, {});
    }
};
