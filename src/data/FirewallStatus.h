#pragma once

#include "ConfFile.h"

#include <QFile>
#include <QString>
#include <QStringView>

// These values describe ufw's configuration files only. They do not prove that
// ufw is running or that its rules are enforced by the active firewall backend.
struct UfwConfiguredState {
    bool enabledKnown = false;
    bool enabled = false;
    bool inputPolicyKnown = false;
    QString inputPolicy;
    bool outputPolicyKnown = false;
    QString outputPolicy;
    bool logLevelKnown = false;
    QString logLevel;
};

inline UfwConfiguredState parseUfwConfiguredState(
    const QString &ufwConf, bool ufwConfReadable,
    const QString &defaultConf, bool defaultConfReadable)
{
    UfwConfiguredState state;
    const ConfField enabled = parseConfFieldText(
        ufwConf, QStringLiteral("ENABLED"), ufwConfReadable);
    if (enabled.available) {
        const QString value = enabled.value.trimmed().toLower();
        if (value == QLatin1String("yes") || value == QLatin1String("no")) {
            state.enabledKnown = true;
            state.enabled = value == QLatin1String("yes");
        }
    }

    const ConfField input = parseConfFieldText(
        defaultConf, QStringLiteral("DEFAULT_INPUT_POLICY"), defaultConfReadable);
    if (input.available) {
        const QString value = input.value.trimmed().toUpper();
        if (value == QLatin1String("ACCEPT") || value == QLatin1String("DROP")
            || value == QLatin1String("REJECT")) {
            state.inputPolicyKnown = true;
            state.inputPolicy = value;
        }
    }

    const ConfField output = parseConfFieldText(
        defaultConf, QStringLiteral("DEFAULT_OUTPUT_POLICY"), defaultConfReadable);
    if (output.available) {
        const QString value = output.value.trimmed().toUpper();
        if (value == QLatin1String("ACCEPT") || value == QLatin1String("DROP")
            || value == QLatin1String("REJECT")) {
            state.outputPolicyKnown = true;
            state.outputPolicy = value;
        }
    }

    const ConfField log = parseConfFieldText(
        ufwConf, QStringLiteral("LOGLEVEL"), ufwConfReadable);
    if (log.available) {
        const QString value = log.value.trimmed().toLower();
        if (value == QLatin1String("off") || value == QLatin1String("low")
            || value == QLatin1String("medium") || value == QLatin1String("high")
            || value == QLatin1String("full")) {
            state.logLevelKnown = true;
            state.logLevel = value;
        }
    }
    return state;
}

inline UfwConfiguredState readUfwConfiguredState()
{
    QFile ufwFile(QStringLiteral("/etc/ufw/ufw.conf"));
    const bool ufwReadable = ufwFile.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString ufwConf = ufwReadable ? QString::fromUtf8(ufwFile.readAll()) : QString();

    QFile defaultFile(QStringLiteral("/etc/default/ufw"));
    const bool defaultReadable = defaultFile.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString defaultConf = defaultReadable
        ? QString::fromUtf8(defaultFile.readAll()) : QString();
    return parseUfwConfiguredState(ufwConf, ufwReadable,
                                   defaultConf, defaultReadable);
}

struct UfwRuleCount {
    int count = 0;
};

inline UfwRuleCount parseUfwRuleCount(const QString &contents)
{
    UfwRuleCount result;
    for (const QStringView &line : QStringView(contents).split(u'\n'))
        if (line.trimmed().startsWith(QLatin1String("### tuple ###")))
            ++result.count;
    return result;
}
