#pragma once
#include "settings/PowerPolicyFields.h"
#include <QCryptographicHash>

// Private fake data only: production always obtains provenance from KConfig.
inline QByteArray fakePowerRevision(PowerPolicyKey key, const QVariant &value, bool present)
{
    return QCryptographicHash::hash(QByteArray::number(int(key))+":"+value.toString().toUtf8()
        +":"+QByteArray::number(present),QCryptographicHash::Sha256);
}
inline void seedPowerPolicyMetadata(PowerPolicySnapshot &snapshot)
{
    auto state=[](PowerPolicyKey key,const QVariant &value,bool editable,const QString &reason) {
        PowerPolicyKeyState s; s.valueKnown=s.defaultKnown=s.effectivePresent=s.localPresent=true;
        s.defaultValue=value; s.writable=editable; s.reason=reason;
        s.resetReason=QStringLiteral("Already equals the native editor default; KDE leaves this override unchanged.");
        s.revision=fakePowerRevision(key,value,true); s.inheritedRevision="fake-inherited-none"; s.globalRevision="fake-global-none";
        return s;
    };
    for (auto &p:snapshot.profiles) {
        p.keys.clear();
        for (auto key:PowerPolicyFields::profileKeys()) {
            bool editable=false; QString reason;
            switch(key) {
            case PowerPolicyKey::DisplayEnabled: case PowerPolicyKey::DisplaySeconds: editable=p.displayEditable;reason=p.displayReason;break;
            case PowerPolicyKey::IdleAction: case PowerPolicyKey::IdleSeconds: editable=p.sleepEditable;reason=p.sleepReason;break;
            case PowerPolicyKey::DimEnabled: case PowerPolicyKey::DimSeconds: editable=p.dimEditable;reason=p.dimReason;break;
            case PowerPolicyKey::BrightnessEnabled: case PowerPolicyKey::BrightnessPercent: editable=p.brightnessEditable;reason=p.brightnessReason;break;
            case PowerPolicyKey::KeyboardBrightnessEnabled: case PowerPolicyKey::KeyboardBrightnessPercent: editable=p.keyboardBrightnessEditable;reason=p.keyboardBrightnessReason;break;
            case PowerPolicyKey::LockedDisplaySeconds: editable=p.lockedDisplayEditable;reason=p.lockedDisplayReason;break;
            case PowerPolicyKey::LockBeforeDisplayOff: editable=p.lockBeforeDisplayOffEditable;reason=p.lockBeforeDisplayOffReason;break;
            case PowerPolicyKey::PowerButtonAction: editable=p.powerButtonEditable;reason=p.powerButtonReason;break;
            case PowerPolicyKey::PowerDownAction: editable=p.powerDownEditable;reason=p.powerDownReason;break;
            case PowerPolicyKey::LidAction: editable=p.lidEditable;reason=p.lidReason;break;
            case PowerPolicyKey::LidExternalMonitorInhibition: editable=p.lidExternalMonitorEditable;reason=p.lidExternalMonitorReason;break;
            case PowerPolicyKey::SleepMode: editable=p.sleepModeEditable;reason=p.sleepModeReason;break;
            default:break;
            }
            p.keys.insert(key,state(key,PowerPolicyFields::value(p,key),editable,reason));
        }
    }
    snapshot.keys.clear();
    for (auto key:PowerPolicyFields::globalKeys()) {
        const bool action=key==PowerPolicyKey::BatteryCriticalAction;
        snapshot.keys.insert(key,state(key,PowerPolicyFields::value(snapshot,key),action?snapshot.batteryCriticalActionEditable:snapshot.batteryLevelsEditable,
            action?snapshot.batteryCriticalActionReason:snapshot.batteryLevelsReason));
    }
}
inline PowerPolicySnapshot savedPowerPolicyRequest(const PowerPolicySnapshot &request)
{
    auto actual=request;
    auto save=[](auto &object) {
        for(auto it=object.intents.cbegin();it!=object.intents.cend();++it) {
            auto &state=object.keys[it.key()]; const bool set=it.value()==PowerPolicyIntent::Set;
            const bool local=set||state.inheritedPresent;
            if(!set&&!state.inheritedPresent&&!state.localPresent) state.globalPresent=false;
            state.valueKnown=true; state.localPresent=state.effectivePresent=local;
            state.revision=fakePowerRevision(it.key(),PowerPolicyFields::value(object,it.key()),local);
            state.resettable=state.writable&&local&&PowerPolicyFields::value(object,it.key())!=state.defaultValue;
            state.resetReason=state.resettable?QString():QStringLiteral("Already uses the native default.");
        }
        object.intents.clear();
    };
    for(auto &p:actual.profiles) save(p); save(actual); return actual;
}
