#pragma once
#include "settings/NightTimeSettings.h"
inline NightTimeSnapshot nightTimeFixture()
{
    NightTimeSnapshot s; s.revision = "fixture-revision";
    s.runtime.owner = QStringLiteral(":1.234"); s.runtime.epoch = 1;
    s.runtime.version = QStringLiteral("6.6.5"); s.runtime.apiVersion = 1;
    s.runtime.installedVerified = s.runtime.serviceAvailable = s.runtime.providerVerified = true;
    for (auto key : NightTimeTypes::keys()) {
        auto &f = s.fields[key]; f.value = f.defaultValue = NightTimeTypes::builtinDefault(key);
        f.known = f.defaultKnown = f.defaultNoOp = f.effectivePresent = f.localPresent = f.writable = true;
        f.revision = "fixture-key-" + QByteArray::number(int(key));
        f.inheritedRevision = "no-inherited"; f.globalRevision = "no-global";
    }
    s.daemonFields = s.fields; return s;
}
inline NightTimeSnapshot savedNightTimeFixture(const NightTimeSnapshot &request)
{
    auto actual = request;
    const auto projected = NightTimeTypes::projectedDaemonFields(request, request);
    for (auto i = request.intents.cbegin(); i != request.intents.cend(); ++i) {
        auto &f = actual.fields[i.key()];
        if (i.value() == NightTimeIntent::RestoreDefault && !f.inheritedPresent) {
            if (!f.localPresent) { f.globalPresent = false; f.globalRevision = "removed-global"; }
            f.localPresent = f.effectivePresent = false;
        } else f.localPresent = f.effectivePresent = true;
        f.known = true; f.defaultNoOp = f.value == f.defaultValue; f.resettable = f.writable && !f.defaultNoOp;
        f.revision += "-saved";
        auto &daemon = actual.daemonFields[i.key()];
        daemon.value = projected.value(i.key()).value; daemon.known = projected.value(i.key()).known;
        daemon.revision += "-saved";
    }
    actual.intents.clear(); actual.revision += "-saved"; return actual;
}
