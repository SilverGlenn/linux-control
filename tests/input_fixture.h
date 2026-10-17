#pragma once
#include "settings/InputSettings.h"

class IntegrationInputBackend final : public InputBackend {
public:
    InputSnapshot current, requested;
    quint64 pendingToken = 0;
    bool hold = false;
    int writes = 0;
    IntegrationInputBackend() {
        current.available = current.writable = true; current.owner = QStringLiteral(":1.77"); current.version = QStringLiteral("6.6.6");
        current.devices = {{QStringLiteral("event1"), QStringLiteral("Fixture mouse"), QStringLiteral("/org/kde/KWin/InputDevice/event1"), {
            {QStringLiteral("name"), QStringLiteral("Fixture mouse")}, {QStringLiteral("sysName"), QStringLiteral("event1")},
            {QStringLiteral("vendor"), 1u}, {QStringLiteral("product"), 2u}, {QStringLiteral("pointer"), true},
            {QStringLiteral("touchpad"), false}, {QStringLiteral("isVirtual"), false},
            {QStringLiteral("leftHanded"), false}, {QStringLiteral("supportsLeftHanded"), true},
            {QStringLiteral("leftHandedEnabledByDefault"), false},
            {QStringLiteral("__writable"), QStringList{QStringLiteral("leftHanded")}}
        }}};
    }
    void readCurrent(quint64 token) override { emit readFinished(token, true, current, {}); }
    void writeChanges(quint64 token, const InputSnapshot &, const InputSnapshot &next) override {
        ++writes; pendingToken = token; requested = next;
        if (!hold) complete();
    }
    void complete() { current = requested; emit writeFinished(pendingToken, InputOutcome::Success, current, {}); }
};
