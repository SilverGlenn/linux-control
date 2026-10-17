#pragma once
#include "settings/ScreenLockSettings.h"
inline ScreenLockSnapshot screenLockFixture()
{
    ScreenLockSnapshot s; s.revision = "fixture-revision";
    s.runtime.owner = QStringLiteral(":1.234"); s.runtime.epoch = 1;
    s.runtime.version = QStringLiteral("6.6.5");
    s.runtime.installedVerified = s.runtime.serviceAvailable = s.runtime.providerVerified = true;
    for (auto key : ScreenLockTypes::keys()) {
        auto &f = s.fields[key]; f.value = f.defaultValue = ScreenLockTypes::builtinDefault(key);
        f.known = f.defaultKnown = f.defaultNoOp = f.effectivePresent = f.localPresent = f.writable = true;
        if (!ScreenLockTypes::owned(key)) f.writable = false;
        f.revision = "fixture-key-" + QByteArray::number(int(key));
        f.inheritedRevision = "no-inherited"; f.globalRevision = "no-global";
    }
    return s;
}
inline ScreenLockSnapshot savedScreenLockFixture(const ScreenLockSnapshot &request)
{
    auto actual = request;
    for (auto i = request.intents.cbegin(); i != request.intents.cend(); ++i) {
        auto &f = actual.fields[i.key()];
        if (i.value() == ScreenLockIntent::RestoreDefault && !f.inheritedPresent) {
            if (!f.localPresent) { f.globalPresent = false; f.globalRevision = "removed-global"; }
            f.localPresent = f.effectivePresent = false;
        } else f.localPresent = f.effectivePresent = true;
        f.known = true; f.defaultNoOp = f.value == f.defaultValue; f.resettable = f.writable && !f.defaultNoOp;
        f.revision += "-saved";

    }
    actual.intents.clear(); actual.revision += "-saved"; return actual;
}
