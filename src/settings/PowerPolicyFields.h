#pragma once
#include "PowerPolicySettings.h"

namespace PowerPolicyFields {
QList<PowerPolicyKey> profileKeys();
QList<PowerPolicyKey> globalKeys();
QString keyName(PowerPolicyKey key);
QString keyGroup(PowerPolicyKey key);
QString label(PowerPolicyKey key);
QVariant value(const PowerPolicyProfile &profile, PowerPolicyKey key);
QVariant value(const PowerPolicySnapshot &snapshot, PowerPolicyKey key);
bool setValue(PowerPolicyProfile &profile, PowerPolicyKey key, const QVariant &value);
bool setValue(PowerPolicySnapshot &snapshot, PowerPolicyKey key, const QVariant &value);
bool validSet(const PowerPolicyProfile &profile, PowerPolicyKey key, const QVariant &value, QString &error);
bool validSet(const PowerPolicySnapshot &snapshot, PowerPolicyKey key, const QVariant &value, QString &error);
bool stage(PowerPolicyProfile &profile, const PowerPolicyProfile &baseline, PowerPolicyKey key, const QVariant &value, QString &error);
bool stage(PowerPolicySnapshot &snapshot, const PowerPolicySnapshot &baseline, PowerPolicyKey key, const QVariant &value, QString &error);
bool restore(PowerPolicyProfile &profile, const PowerPolicyProfile &baseline, PowerPolicyKey key, QString &error);
bool restore(PowerPolicySnapshot &snapshot, const PowerPolicySnapshot &baseline, PowerPolicyKey key, QString &error);
void clearIntents(PowerPolicySnapshot &snapshot);
bool hasIntents(const PowerPolicySnapshot &snapshot);
bool requestValid(const PowerPolicySnapshot &expected, const PowerPolicySnapshot &requested, QString &error);
bool matchesRequest(const PowerPolicySnapshot &expected, const PowerPolicySnapshot &requested, const PowerPolicySnapshot &actual, QString &error);
QByteArray revision(const PowerPolicySnapshot &snapshot);
}
