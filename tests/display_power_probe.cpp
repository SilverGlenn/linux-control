// Manual read-only discovery. No setters, Apply, reload or inhibition calls.
#include "display/KdeDisplayBackend.h"
#include "platform/kde/PowerPolicyBackend.h"
#include "settings/PowerPolicyFields.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>

namespace {
QJsonArray keys(const QMap<PowerPolicyKey,PowerPolicyKeyState> &values)
{
    QJsonArray result;
    for(auto it=values.cbegin();it!=values.cend();++it) {
        const auto &state=it.value();
        result.append(QJsonObject{{"key",PowerPolicyFields::keyName(it.key())}, {"group",PowerPolicyFields::keyGroup(it.key())},
            {"valueKnown",state.valueKnown},{"defaultKnown",state.defaultKnown},
            {"effectivePresent",state.effectivePresent},{"localPresent",state.localPresent},
            {"inheritedPresent",state.inheritedPresent},{"globalPresent",state.globalPresent},
            {"immutable",state.immutable},{"writable",state.writable},{"resettable",state.resettable},
            {"nativeEditorDefault",state.defaultKnown?QJsonValue::fromVariant(state.defaultValue):QJsonValue(QJsonValue::Null)},
            {"reason",state.reason},{"defaultReason",state.defaultReason},{"resetReason",state.resetReason},
            {"revision",QString::fromLatin1(state.revision.toHex())}});
    }
    return result;
}
QJsonArray actions(const QList<PowerPolicyAction> &values)
{
    QJsonArray result;
    for (const auto &value : values)
        result.append(QJsonObject{{"value", int(value.value)}, {"label", value.label}});
    return result;
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    DisplaySettings display(new KdeDisplayBackend);
    PowerPolicySettings power(new KdePowerPolicyBackend);
    bool timedOut = false;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (!display.isBusy() && !power.isBusy()) app.quit();
    });
    poll.start(50);
    QTimer::singleShot(12000, &app, [&] { timedOut = true; app.quit(); });
    app.exec();
    QJsonArray outputs;
    for (const auto &o : display.snapshot().outputs) {
        QString editReason;
        const bool editable = DisplayProtocol::canEdit(display.snapshot(), o.id, editReason);
        outputs.append(QJsonObject{{"id", o.id}, {"name", o.name}, {"connected", o.connected},
            {"enabled", o.enabled}, {"mode", o.currentMode}, {"modes", o.modes.size()},
            {"scale", o.scale}, {"rotation", o.rotation}, {"priority", o.priority},
            {"x", o.position.x()}, {"y", o.position.y()}, {"followPreferredMode", o.followPreferredMode},
            {"editable", editable}, {"reason", editReason}});
    }
    QJsonArray profiles;
    for (const auto &p : power.applied().profiles) {
        profiles.append(QJsonObject{{"id", p.id}, {"keys",keys(p.keys)}, {"displayEnabled", p.displayEnabled},
            {"displaySeconds", p.displaySeconds}, {"displayEditable", p.displayEditable}, {"displayReason", p.displayReason},
            {"idleAction", int(p.suspendAction)}, {"idleSeconds", p.suspendSeconds},
            {"idleEditable", p.sleepEditable}, {"idleReason", p.sleepReason}, {"idleActions", actions(p.idleActions)},
            {"dimEnabled", p.dimEnabled}, {"dimSeconds", p.dimSeconds}, {"dimEditable", p.dimEditable}, {"dimReason", p.dimReason},
            {"brightnessEnabled", p.brightnessEnabled}, {"brightnessPercent", p.brightnessPercent},
            {"brightnessEditable", p.brightnessEditable}, {"brightnessReason", p.brightnessReason},
            {"keyboardBrightnessEnabled", p.keyboardBrightnessEnabled}, {"keyboardBrightnessPercent", p.keyboardBrightnessPercent},
            {"keyboardBrightnessEditable", p.keyboardBrightnessEditable}, {"keyboardBrightnessReason", p.keyboardBrightnessReason},
            {"lockedDisplaySeconds", p.lockedDisplaySeconds}, {"lockedDisplayEditable", p.lockedDisplayEditable},
            {"lockedDisplayReason", p.lockedDisplayReason}, {"lockBeforeDisplayOff", p.lockBeforeDisplayOff},
            {"lockBeforeDisplayOffEditable", p.lockBeforeDisplayOffEditable}, {"lockBeforeDisplayOffReason", p.lockBeforeDisplayOffReason},
            {"powerButtonAction", int(p.powerButtonAction)}, {"powerButtonEditable", p.powerButtonEditable}, {"powerButtonReason", p.powerButtonReason},
            {"powerDownAction", int(p.powerDownAction)}, {"powerDownEditable", p.powerDownEditable}, {"powerDownReason", p.powerDownReason},
            {"powerButtonActions", actions(p.powerButtonActions)}, {"lidAction", int(p.lidAction)}, {"lidEditable", p.lidEditable},
            {"lidReason", p.lidReason}, {"lidActions", actions(p.lidActions)},
            {"inhibitLidWithExternalMonitor", p.inhibitLidActionWhenExternalMonitorPresent},
            {"lidExternalMonitorEditable", p.lidExternalMonitorEditable}, {"lidExternalMonitorReason", p.lidExternalMonitorReason},
            {"sleepMode", int(p.sleepMode)}, {"sleepModeEditable", p.sleepModeEditable},
            {"sleepModeReason", p.sleepModeReason}, {"sleepModes", actions(p.sleepModes)}});
    }
    const auto &p = power.applied();
    QJsonObject report{{"mutationPerformed", false}, {"timedOut", timedOut},
        {"display", QJsonObject{{"ready", display.isReady()}, {"outputs", outputs},
            {"rollbackAvailable", display.rollbackAvailable()}, {"support", display.supportText()},
            {"scaleSupported", DisplayProtocol::canScale(display.snapshot())},
            {"rotationSupported", DisplayProtocol::canRotate(display.snapshot())},
            {"positionSupported", DisplayProtocol::canPosition(display.snapshot())},
            {"identity", display.snapshot().json.value("controlIdentity")}, {"error", display.errorText()}}},
        {"power", QJsonObject{{"ready", power.isReady()}, {"serviceAvailable", p.serviceAvailable},
            {"owner", p.owner}, {"capabilityRevision", p.capabilityRevision}, {"profiles", profiles}, {"keys",keys(p.keys)},
            {"lowBatteryLevel", p.batteryLowLevel}, {"criticalBatteryLevel", p.batteryCriticalLevel},
            {"batteryLevelsEditable", p.batteryLevelsEditable}, {"batteryLevelsReason", p.batteryLevelsReason},
            {"criticalAction", int(p.batteryCriticalAction)}, {"criticalActions", actions(p.batteryCriticalActions)},
            {"criticalActionEditable", p.batteryCriticalActionEditable}, {"criticalActionReason", p.batteryCriticalActionReason},
            {"reason", p.unavailableReason}, {"error", power.errorText()}}}};
    QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
    return timedOut || !display.isReady() || !power.isReady() ? 1 : 0;
}
