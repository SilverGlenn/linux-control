#pragma once
#include "settings/NightLightSettings.h"

inline void observeNightLightFixture(NightLightSnapshot &snapshot)
{
    auto &r = snapshot.runtime;
    r.enabled = snapshot.fields[NightLightKey::Active].value.toBool();
    const int mode = snapshot.fields[NightLightKey::Mode].value.toInt();
    r.mode = uint(mode == 0 ? 0 : 1); r.running = r.enabled && !r.inhibited;
    if (!r.enabled) r.daylight = true;
    else if (r.mode == 0) r.daylight = false;
    const auto key = r.mode != 0 && r.daylight ? NightLightKey::DayTemperature : NightLightKey::NightTemperature;
    r.currentTemperature = r.targetTemperature = uint(qBound(1000, snapshot.fields[key].value.toInt(), 6500));
}
inline NightLightSnapshot nightLightFixture()
{
    NightLightSnapshot s;
    s.revision = "fixture-source-revision";
    s.runtime.owner = QStringLiteral(":1.123"); s.runtime.epoch = 1; s.runtime.version = QStringLiteral("6.6.6");
    s.runtime.serviceAvailable = s.runtime.schemaSupported = s.runtime.known = s.runtime.available = s.runtime.previewSupported = true;
    s.runtime.daylight = true;
    for (const auto key : NightLightTypes::keys()) {
        auto &f = s.fields[key]; f.value = f.defaultValue = NightLightTypes::builtinDefault(key);
        f.known = f.defaultKnown = f.defaultNoOp = f.localPresent = f.effectivePresent = f.writable = true;
        f.inheritedRevision = "no-inherited"; f.globalRevision = "no-global";
        f.revision = "fixture-key-" + QByteArray::number(int(key));
    }
    s.fields[NightLightKey::Active].value = true; s.fields[NightLightKey::Active].defaultNoOp = false;
    s.fields[NightLightKey::Active].resettable = true;
    observeNightLightFixture(s); return s;
}
inline NightLightSnapshot savedNightLightFixture(const NightLightSnapshot &request)
{
    auto actual = request;
    for (auto i = request.intents.cbegin(); i != request.intents.cend(); ++i) {
        auto &f = actual.fields[i.key()];
        if (i.value() == NightLightIntent::RestoreDefault && !f.inheritedPresent) {
            if (!f.localPresent) { f.globalPresent = false; f.globalRevision = "removed-global"; }
            f.localPresent = f.effectivePresent = false;
        } else f.localPresent = f.effectivePresent = true;
        f.known = true;
        f.defaultNoOp = f.value == f.defaultValue;
        f.resettable = f.writable && !f.defaultNoOp;
        f.revision += "-saved";
    }
    actual.intents.clear(); actual.revision += "-saved";
    observeNightLightFixture(actual); return actual;
}
