#include "PowerPolicyFields.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QSet>
#include <type_traits>

namespace PowerPolicyFields {
QList<PowerPolicyKey> profileKeys()
{
    return {PowerPolicyKey::DisplayEnabled, PowerPolicyKey::DisplaySeconds,
        PowerPolicyKey::IdleAction, PowerPolicyKey::IdleSeconds,
        PowerPolicyKey::DimEnabled, PowerPolicyKey::DimSeconds,
        PowerPolicyKey::BrightnessEnabled, PowerPolicyKey::BrightnessPercent,
        PowerPolicyKey::KeyboardBrightnessEnabled, PowerPolicyKey::KeyboardBrightnessPercent,
        PowerPolicyKey::LockedDisplaySeconds, PowerPolicyKey::LockBeforeDisplayOff,
        PowerPolicyKey::PowerButtonAction, PowerPolicyKey::PowerDownAction,
        PowerPolicyKey::LidAction, PowerPolicyKey::LidExternalMonitorInhibition, PowerPolicyKey::SleepMode};
}
QList<PowerPolicyKey> globalKeys()
{ return {PowerPolicyKey::BatteryLowLevel, PowerPolicyKey::BatteryCriticalLevel, PowerPolicyKey::BatteryCriticalAction}; }
QString keyName(PowerPolicyKey key)
{
    switch (key) {
    case PowerPolicyKey::DisplayEnabled: return QStringLiteral("TurnOffDisplayWhenIdle");
    case PowerPolicyKey::DisplaySeconds: return QStringLiteral("TurnOffDisplayIdleTimeoutSec");
    case PowerPolicyKey::IdleAction: return QStringLiteral("AutoSuspendAction");
    case PowerPolicyKey::IdleSeconds: return QStringLiteral("AutoSuspendIdleTimeoutSec");
    case PowerPolicyKey::DimEnabled: return QStringLiteral("DimDisplayWhenIdle");
    case PowerPolicyKey::DimSeconds: return QStringLiteral("DimDisplayIdleTimeoutSec");
    case PowerPolicyKey::BrightnessEnabled: return QStringLiteral("UseProfileSpecificDisplayBrightness");
    case PowerPolicyKey::BrightnessPercent: return QStringLiteral("DisplayBrightness");
    case PowerPolicyKey::KeyboardBrightnessEnabled: return QStringLiteral("UseProfileSpecificKeyboardBrightness");
    case PowerPolicyKey::KeyboardBrightnessPercent: return QStringLiteral("KeyboardBrightness");
    case PowerPolicyKey::LockedDisplaySeconds: return QStringLiteral("TurnOffDisplayIdleTimeoutWhenLockedSec");
    case PowerPolicyKey::LockBeforeDisplayOff: return QStringLiteral("LockBeforeTurnOffDisplay");
    case PowerPolicyKey::PowerButtonAction: return QStringLiteral("PowerButtonAction");
    case PowerPolicyKey::PowerDownAction: return QStringLiteral("PowerDownAction");
    case PowerPolicyKey::LidAction: return QStringLiteral("LidAction");
    case PowerPolicyKey::LidExternalMonitorInhibition: return QStringLiteral("InhibitLidActionWhenExternalMonitorPresent");
    case PowerPolicyKey::SleepMode: return QStringLiteral("SleepMode");
    case PowerPolicyKey::BatteryLowLevel: return QStringLiteral("BatteryLowLevel");
    case PowerPolicyKey::BatteryCriticalLevel: return QStringLiteral("BatteryCriticalLevel");
    case PowerPolicyKey::BatteryCriticalAction: return QStringLiteral("BatteryCriticalAction");
    }
    return {};
}
QString keyGroup(PowerPolicyKey key)
{
    if (globalKeys().contains(key)) return QStringLiteral("BatteryManagement");
    switch (key) {
    case PowerPolicyKey::KeyboardBrightnessEnabled: case PowerPolicyKey::KeyboardBrightnessPercent: return QStringLiteral("Keyboard");
    case PowerPolicyKey::IdleAction: case PowerPolicyKey::IdleSeconds:
    case PowerPolicyKey::PowerButtonAction: case PowerPolicyKey::PowerDownAction:
    case PowerPolicyKey::LidAction: case PowerPolicyKey::LidExternalMonitorInhibition:
    case PowerPolicyKey::SleepMode: return QStringLiteral("SuspendAndShutdown");
    default: return profileKeys().contains(key) ? QStringLiteral("Display") : QString();
    }
}
QString label(PowerPolicyKey key)
{
    switch (key) {
    case PowerPolicyKey::DisplayEnabled: return QStringLiteral("Turn off display when idle");
    case PowerPolicyKey::DisplaySeconds: return QStringLiteral("Display timeout");
    case PowerPolicyKey::IdleAction: return QStringLiteral("Idle action");
    case PowerPolicyKey::IdleSeconds: return QStringLiteral("Idle action timeout");
    case PowerPolicyKey::DimEnabled: return QStringLiteral("Dim display when idle");
    case PowerPolicyKey::DimSeconds: return QStringLiteral("Dim display timeout");
    case PowerPolicyKey::BrightnessEnabled: return QStringLiteral("Use profile display brightness");
    case PowerPolicyKey::BrightnessPercent: return QStringLiteral("Display brightness");
    case PowerPolicyKey::KeyboardBrightnessEnabled: return QStringLiteral("Use profile keyboard brightness");
    case PowerPolicyKey::KeyboardBrightnessPercent: return QStringLiteral("Keyboard brightness");
    case PowerPolicyKey::LockedDisplaySeconds: return QStringLiteral("Locked display timeout");
    case PowerPolicyKey::LockBeforeDisplayOff: return QStringLiteral("Lock before turning off display");
    case PowerPolicyKey::PowerButtonAction: return QStringLiteral("Power button action");
    case PowerPolicyKey::PowerDownAction: return QStringLiteral("Long power-button press action");
    case PowerPolicyKey::LidAction: return QStringLiteral("Lid action");
    case PowerPolicyKey::LidExternalMonitorInhibition: return QStringLiteral("Inhibit lid action with an external monitor");
    case PowerPolicyKey::SleepMode: return QStringLiteral("Sleep mode");
    case PowerPolicyKey::BatteryLowLevel: return QStringLiteral("Low battery level");
    case PowerPolicyKey::BatteryCriticalLevel: return QStringLiteral("Critical battery level");
    case PowerPolicyKey::BatteryCriticalAction: return QStringLiteral("Critical battery action");
    }
    return {};
}
QVariant value(const PowerPolicyProfile &p, PowerPolicyKey key)
{
    switch (key) {
    case PowerPolicyKey::DisplayEnabled: return p.displayEnabled;
    case PowerPolicyKey::DisplaySeconds: return p.displaySeconds;
    case PowerPolicyKey::IdleAction: return p.suspendAction;
    case PowerPolicyKey::IdleSeconds: return p.suspendSeconds;
    case PowerPolicyKey::DimEnabled: return p.dimEnabled;
    case PowerPolicyKey::DimSeconds: return p.dimSeconds;
    case PowerPolicyKey::BrightnessEnabled: return p.brightnessEnabled;
    case PowerPolicyKey::BrightnessPercent: return p.brightnessPercent;
    case PowerPolicyKey::KeyboardBrightnessEnabled: return p.keyboardBrightnessEnabled;
    case PowerPolicyKey::KeyboardBrightnessPercent: return p.keyboardBrightnessPercent;
    case PowerPolicyKey::LockedDisplaySeconds: return p.lockedDisplaySeconds;
    case PowerPolicyKey::LockBeforeDisplayOff: return p.lockBeforeDisplayOff;
    case PowerPolicyKey::PowerButtonAction: return p.powerButtonAction;
    case PowerPolicyKey::PowerDownAction: return p.powerDownAction;
    case PowerPolicyKey::LidAction: return p.lidAction;
    case PowerPolicyKey::LidExternalMonitorInhibition: return p.inhibitLidActionWhenExternalMonitorPresent;
    case PowerPolicyKey::SleepMode: return p.sleepMode;
    default: return {};
    }
}
QVariant value(const PowerPolicySnapshot &s, PowerPolicyKey key)
{
    switch (key) {
    case PowerPolicyKey::BatteryLowLevel: return s.batteryLowLevel;
    case PowerPolicyKey::BatteryCriticalLevel: return s.batteryCriticalLevel;
    case PowerPolicyKey::BatteryCriticalAction: return s.batteryCriticalAction;
    default: return {};
    }
}
namespace {
bool exact(const QVariant &a, const QVariant &b) { return a.metaType() == b.metaType() && a == b; }
bool offers(const QList<PowerPolicyAction> &choices, uint candidate)
{ for (const auto &choice : choices) if (choice.value == candidate) return true; return false; }
template<class T> bool typed(const T &object, PowerPolicyKey key, const QVariant &candidate)
{ const auto current = value(object, key); return current.isValid() && current.metaType() == candidate.metaType(); }
bool fail(QString &error, PowerPolicyKey key, const QString &why)
{ error = QStringLiteral("%1: %2").arg(label(key), why); return false; }
template<class T> bool writable(const T &object, PowerPolicyKey key, const QVariant &candidate, QString &error)
{
    if (!typed(object, key, candidate)) return fail(error, key, QStringLiteral("the value has the wrong native type."));
    const auto it = object.keys.constFind(key);
    if (it == object.keys.cend() || !it->writable || it->immutable)
        return fail(error, key, it == object.keys.cend() || it->reason.isEmpty()
            ? QStringLiteral("this native setting is unavailable.") : it->reason);
    return true;
}
template<class T> bool stageImpl(T &object, const T &baseline, PowerPolicyKey key, const QVariant &candidate, QString &error)
{
    if (!validSet(object, key, candidate, error)) return false;
    const auto state = baseline.keys.value(key);
    setValue(object, key, candidate);
    if (state.valueKnown && state.localPresent && exact(value(baseline, key), candidate)) object.intents.remove(key);
    else object.intents.insert(key, PowerPolicyIntent::Set);
    return true;
}
template<class T> bool restoreImpl(T &object, const T &baseline, PowerPolicyKey key, QString &error)
{
    const auto it = baseline.keys.constFind(key);
    if (it == baseline.keys.cend()) return fail(error, key, QStringLiteral("the native default has not been read."));
    const auto &state = *it;
    // A native editor does not save unchanged defaults. Cancelling an unapplied
    // Set on such a key restores the baseline without inventing a file write.
    if (!state.effectivePresent || (state.valueKnown && state.defaultKnown && exact(value(baseline, key), state.defaultValue))) {
        if (!object.intents.contains(key)) return fail(error, key, state.resetReason);
        setValue(object, key, value(baseline, key)); object.intents.remove(key); return true;
    }
    if (!state.resettable || !state.defaultKnown || !state.writable || state.immutable
        || !typed(baseline, key, state.defaultValue))
        return fail(error, key, state.resetReason.isEmpty() ? QStringLiteral("the native default is unavailable.") : state.resetReason);
    setValue(object, key, state.defaultValue); object.intents.insert(key, PowerPolicyIntent::RestoreDefault); return true;
}
template<class T> bool validObject(const T &expected, const T &requested, const QList<PowerPolicyKey> &allowed, QString &error)
{
    if (expected.keys.keys() != allowed || requested.keys != expected.keys)
        return error = QStringLiteral("Native power setting metadata changed or is incomplete."), false;
    for (auto it = requested.intents.cbegin(); it != requested.intents.cend(); ++it) {
        const auto key = it.key();
        if (!allowed.contains(key)) return fail(error, key, QStringLiteral("this key is not part of the requested scope."));
        if (it.value() == PowerPolicyIntent::Set) {
            if (!validSet(requested, key, value(requested, key), error)) return false;
        } else if (it.value() == PowerPolicyIntent::RestoreDefault) {
            const auto state = expected.keys.value(key);
            if (!state.resettable || !state.defaultKnown || !state.writable || state.immutable
                || !typed(expected, key, state.defaultValue) || !exact(value(requested, key), state.defaultValue))
                return fail(error, key, QStringLiteral("the reviewed native default is no longer available."));
        } else return fail(error, key, QStringLiteral("unknown write intent."));
    }
    auto comparable = requested; comparable.intents.clear();
    for (auto key : allowed) if (requested.intents.contains(key)) setValue(comparable, key, value(expected, key));
    auto baseline = expected; baseline.intents.clear();
    // Snapshot equality also covers profiles; the caller compares their whole
    // sanitized snapshot after individual profile validation.
    if constexpr (std::is_same_v<T, PowerPolicyProfile>) {
        if (!(baseline == comparable)) return error = QStringLiteral("An unrequested power value or capability changed."), false;
    }
    return true;
}
template<class T> bool verifyObject(const T &before, const T &request, T &actual, const QList<PowerPolicyKey> &allowed, QString &error)
{
    if (!actual.intents.isEmpty() || actual.keys.keys() != allowed)
        return error = QStringLiteral("The native readback contains invalid setting metadata."), false;
    for (auto it = request.intents.cbegin(); it != request.intents.cend(); ++it) {
        const auto key = it.key(); const auto &old = before.keys[key]; auto &state = actual.keys[key];
        const bool set = it.value() == PowerPolicyIntent::Set;
        if (set || old.inheritedPresent) {
            if (!state.valueKnown || !state.localPresent || !state.effectivePresent
                || !exact(value(actual, key), value(request, key)))
                return fail(error, key, QStringLiteral("the requested native value was not saved as a local override."));
        } else {
            if (state.localPresent || state.effectivePresent)
                return fail(error, key, QStringLiteral("the saved override was not reverted to its native default."));
        }
        const bool globalRemoved = !set && !old.inheritedPresent && !old.localPresent && old.globalPresent;
        if (globalRemoved) {
            if (state.globalPresent) return fail(error, key, QStringLiteral("the native global override was not reverted."));
        } else if (state.globalPresent != old.globalPresent || state.globalRevision != old.globalRevision)
            return fail(error, key, QStringLiteral("an unrelated native global value changed."));
        // The operation owns these derived facts. Immutable policy, native
        // editor defaults, inherited source revisions and capabilities remain
        // guarded by the final full-snapshot comparison below.
        state.valueKnown = old.valueKnown; state.effectivePresent = old.effectivePresent;
        state.localPresent = old.localPresent; state.globalPresent = old.globalPresent;
        state.resettable = old.resettable; state.resetReason = old.resetReason;
        state.revision = old.revision; state.globalRevision = old.globalRevision;
        setValue(actual, key, value(before, key));
    }
    return true;
}
void streamState(QDataStream &out, const PowerPolicyKeyState &s)
{
    out << s.valueKnown << s.defaultKnown << s.effectivePresent << s.localPresent << s.inheritedPresent << s.globalPresent
        << s.immutable << s.writable << s.resettable << s.defaultValue << s.reason << s.defaultReason << s.resetReason
        << s.revision << s.inheritedRevision << s.globalRevision;
}
template<class T> void streamFields(QDataStream &out, const T &object, const QList<PowerPolicyKey> &keys)
{
    for (auto key : keys) { out << int(key) << value(object, key) << object.keys.contains(key); streamState(out, object.keys.value(key)); }
    out << object.intents.size(); for (auto it=object.intents.cbegin();it!=object.intents.cend();++it) out << int(it.key()) << int(it.value());
}
void streamChoices(QDataStream &out, const QList<PowerPolicyAction> &choices)
{ out << choices.size(); for (const auto &choice:choices) out << choice.value << choice.label; }
}
bool setValue(PowerPolicyProfile &p, PowerPolicyKey key, const QVariant &v)
{
    if (!typed(p,key,v)) return false;
    switch (key) {
    case PowerPolicyKey::DisplayEnabled: p.displayEnabled=v.toBool(); break;
    case PowerPolicyKey::DisplaySeconds: p.displaySeconds=v.toInt(); break;
    case PowerPolicyKey::IdleAction: p.suspendAction=v.toUInt(); break;
    case PowerPolicyKey::IdleSeconds: p.suspendSeconds=v.toInt(); break;
    case PowerPolicyKey::DimEnabled: p.dimEnabled=v.toBool(); break;
    case PowerPolicyKey::DimSeconds: p.dimSeconds=v.toInt(); break;
    case PowerPolicyKey::BrightnessEnabled: p.brightnessEnabled=v.toBool(); break;
    case PowerPolicyKey::BrightnessPercent: p.brightnessPercent=v.toInt(); break;
    case PowerPolicyKey::KeyboardBrightnessEnabled: p.keyboardBrightnessEnabled=v.toBool(); break;
    case PowerPolicyKey::KeyboardBrightnessPercent: p.keyboardBrightnessPercent=v.toInt(); break;
    case PowerPolicyKey::LockedDisplaySeconds: p.lockedDisplaySeconds=v.toInt(); break;
    case PowerPolicyKey::LockBeforeDisplayOff: p.lockBeforeDisplayOff=v.toBool(); break;
    case PowerPolicyKey::PowerButtonAction: p.powerButtonAction=v.toUInt(); break;
    case PowerPolicyKey::PowerDownAction: p.powerDownAction=v.toUInt(); break;
    case PowerPolicyKey::LidAction: p.lidAction=v.toUInt(); break;
    case PowerPolicyKey::LidExternalMonitorInhibition: p.inhibitLidActionWhenExternalMonitorPresent=v.toBool(); break;
    case PowerPolicyKey::SleepMode: p.sleepMode=v.toUInt(); break;
    default: return false;
    }
    return true;
}
bool setValue(PowerPolicySnapshot &s, PowerPolicyKey key, const QVariant &v)
{
    if (!typed(s,key,v)) return false;
    switch (key) {
    case PowerPolicyKey::BatteryLowLevel: s.batteryLowLevel=v.toInt(); break;
    case PowerPolicyKey::BatteryCriticalLevel: s.batteryCriticalLevel=v.toInt(); break;
    case PowerPolicyKey::BatteryCriticalAction: s.batteryCriticalAction=v.toUInt(); break;
    default:return false;
    }
    return true;
}
bool validSet(const PowerPolicyProfile &p, PowerPolicyKey key, const QVariant &v, QString &error)
{
    if (!writable(p,key,v,error)) return false;
    bool valid = false;
    switch (key) {
    case PowerPolicyKey::DisplaySeconds: valid=v.toInt()>=30 && v.toInt()<=PowerPolicySettings::MaximumTimeoutSeconds; break;
    case PowerPolicyKey::IdleSeconds: valid=v.toInt()>=60 && v.toInt()<=PowerPolicySettings::MaximumTimeoutSeconds; break;
    case PowerPolicyKey::DimSeconds: valid=v.toInt()>=10 && v.toInt()<=PowerPolicySettings::MaximumTimeoutSeconds; break;
    case PowerPolicyKey::LockedDisplaySeconds: valid=v.toInt()==-2 || v.toInt()==0 || (v.toInt()>=10 && v.toInt()<=PowerPolicySettings::MaximumTimeoutSeconds); break;
    case PowerPolicyKey::BrightnessPercent: case PowerPolicyKey::KeyboardBrightnessPercent: valid=v.toInt()>=0 && v.toInt()<=100; break;
    case PowerPolicyKey::IdleAction: valid=offers(p.idleActions,v.toUInt()); break;
    case PowerPolicyKey::PowerButtonAction: case PowerPolicyKey::PowerDownAction: valid=offers(p.powerButtonActions,v.toUInt()); break;
    case PowerPolicyKey::LidAction: valid=offers(p.lidActions,v.toUInt()); break;
    case PowerPolicyKey::SleepMode: valid=offers(p.sleepModes,v.toUInt()); break;
    default: valid=v.metaType().id()==QMetaType::Bool; break;
    }
    return valid || fail(error,key,QStringLiteral("choose an available native value within the supported range."));
}
bool validSet(const PowerPolicySnapshot &s, PowerPolicyKey key, const QVariant &v, QString &error)
{
    if (!writable(s,key,v,error)) return false;
    const bool valid=key==PowerPolicyKey::BatteryCriticalAction ? offers(s.batteryCriticalActions,v.toUInt()) : v.toInt()>=0 && v.toInt()<=100;
    return valid || fail(error,key,QStringLiteral("choose an available native value within the supported range."));
}
bool stage(PowerPolicyProfile &p,const PowerPolicyProfile &b,PowerPolicyKey k,const QVariant &v,QString &e) { return stageImpl(p,b,k,v,e); }
bool stage(PowerPolicySnapshot &p,const PowerPolicySnapshot &b,PowerPolicyKey k,const QVariant &v,QString &e) { return stageImpl(p,b,k,v,e); }
bool restore(PowerPolicyProfile &p,const PowerPolicyProfile &b,PowerPolicyKey k,QString &e) { return restoreImpl(p,b,k,e); }
bool restore(PowerPolicySnapshot &p,const PowerPolicySnapshot &b,PowerPolicyKey k,QString &e) { return restoreImpl(p,b,k,e); }
void clearIntents(PowerPolicySnapshot &s) { s.intents.clear(); for (auto &p:s.profiles) p.intents.clear(); }
bool hasIntents(const PowerPolicySnapshot &s) { if (!s.intents.isEmpty()) return true; for (const auto &p:s.profiles) if (!p.intents.isEmpty()) return true; return false; }
bool requestValid(const PowerPolicySnapshot &expected,const PowerPolicySnapshot &requested,QString &error)
{
    error.clear();
    if (!expected.serviceAvailable || expected.owner.isEmpty() || expected.capabilityRevision.isEmpty()
        || hasIntents(expected) || expected.profiles.size()!=requested.profiles.size())
        return error=QStringLiteral("The native power baseline is unavailable or invalid."),false;
    if (!validObject(expected,requested,globalKeys(),error)) return false;
    auto comparable=requested; clearIntents(comparable);
    for (auto key:globalKeys()) if(requested.intents.contains(key)) setValue(comparable,key,value(expected,key));
    QSet<QString> ids;
    for (int i=0;i<expected.profiles.size();++i) {
        const auto &before=expected.profiles[i]; const auto &after=requested.profiles[i];
        if (before.id.isEmpty() || ids.contains(before.id) || !validObject(before,after,profileKeys(),error))
            return error.isEmpty() ? error=QStringLiteral("The native profile identity changed."),false : false;
        ids.insert(before.id); comparable.profiles[i]=before;
    }
    if (!(comparable==expected)) return error=QStringLiteral("An unrequested power value or capability changed."),false;
    if (requested.intents.contains(PowerPolicyKey::BatteryLowLevel) || requested.intents.contains(PowerPolicyKey::BatteryCriticalLevel)) {
        auto known=[&](PowerPolicyKey key){return expected.keys.value(key).valueKnown || requested.intents.contains(key);};
        if (!known(PowerPolicyKey::BatteryLowLevel) || !known(PowerPolicyKey::BatteryCriticalLevel))
            return error=QStringLiteral("Choose explicit low and critical battery levels so their ordering can be verified."),false;
        if (requested.batteryCriticalLevel<0 || requested.batteryLowLevel>100)
            return error=QStringLiteral("Choose low and critical battery levels between 0% and 100%; repair the saved companion value before applying."),false;
        if (requested.batteryCriticalLevel>requested.batteryLowLevel)
            return error=QStringLiteral("The critical battery level must not exceed the low battery level."),false;
    }
    return true;
}
bool matchesRequest(const PowerPolicySnapshot &expected,const PowerPolicySnapshot &requested,const PowerPolicySnapshot &actual,QString &error)
{
    if (!requestValid(expected,requested,error)) return false;
    if (actual.profiles.size()!=expected.profiles.size()) return error=QStringLiteral("Native power profiles changed during readback."),false;
    auto comparable=actual;
    if (!verifyObject(expected,requested,comparable,globalKeys(),error)) return false;
    for (int i=0;i<expected.profiles.size();++i)
        if (!verifyObject(expected.profiles[i],requested.profiles[i],comparable.profiles[i],profileKeys(),error)) return false;
    if (!(comparable==expected)) return error=QStringLiteral("An unrequested power value, policy, default or native provider changed during readback."),false;
    return true;
}
QByteArray revision(const PowerPolicySnapshot &s)
{
    QByteArray data; QDataStream out(&data,QIODevice::WriteOnly);
    out << s.serviceAvailable << s.unavailableReason << s.owner << s.capabilityRevision << s.batteryLevelsEditable
        << s.batteryCriticalActionEditable << s.batteryLevelsReason << s.batteryCriticalActionReason;
    streamFields(out,s,globalKeys()); streamChoices(out,s.batteryCriticalActions); out << s.profiles.size();
    for (const auto &p:s.profiles) {
        out << p.id << p.displayEditable << p.sleepEditable << p.displayReason << p.sleepReason
            << p.dimEditable << p.brightnessEditable << p.keyboardBrightnessEditable << p.lockedDisplayEditable
            << p.lockBeforeDisplayOffEditable << p.powerButtonEditable << p.powerDownEditable << p.lidEditable
            << p.lidExternalMonitorEditable << p.sleepModeEditable << p.dimReason << p.brightnessReason
            << p.keyboardBrightnessReason << p.lockedDisplayReason << p.lockBeforeDisplayOffReason
            << p.powerButtonReason << p.powerDownReason << p.lidReason << p.lidExternalMonitorReason << p.sleepModeReason;
        streamFields(out,p,profileKeys()); streamChoices(out,p.idleActions); streamChoices(out,p.powerButtonActions);
        streamChoices(out,p.lidActions); streamChoices(out,p.sleepModes);
    }
    return QCryptographicHash::hash(data,QCryptographicHash::Sha256);
}
}
