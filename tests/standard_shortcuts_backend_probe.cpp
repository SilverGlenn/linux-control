#include <QApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <KSharedConfig>
#include <KStandardShortcut>
#include "platform/shortcuts/StandardShortcutsBackend.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // Read-only: no writer, notification, registration, invocation, or KCM.
    // SDK shortcut() caches only within this fresh, single-snapshot process.
    const auto nativeConfig = KSharedConfig::openConfig();
    const auto nativeName = nativeConfig->name();
    QStringList applicationConfigSources;
    if (QFileInfo(nativeName).isAbsolute()) {
        if (QFileInfo::exists(nativeName)) applicationConfigSources << nativeName;
    } else {
        applicationConfigSources = QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation, nativeName);
        if (QFileInfo::exists(QStringLiteral(":/kconfig/") + nativeName)) applicationConfigSources << QStringLiteral(":/kconfig/") + nativeName;
    }
    const bool applicationOverridePresent = !applicationConfigSources.isEmpty();
    KdeStandardShortcutsBackend backend;
    QObject::connect(&backend, &ShortcutsBackend::readFinished, &app,
                     [&](quint64, bool success, const ShortcutsSnapshot &state, const QString &error) {
        QJsonArray actions, mismatches, applicationSources;
        for (const auto &path : applicationConfigSources) applicationSources << path;
        int compared = 0;
        for (const auto &action : state.actions) {
            QJsonArray current, defaults, sdkCurrent;
            for (const auto &key : action.current) current << key.toString(QKeySequence::PortableText);
            for (const auto &key : action.defaults) defaults << key.toString(QKeySequence::PortableText);
            const auto nativeId = KStandardShortcut::findByName(action.id.action);
            const auto sdk = KStandardShortcut::shortcut(nativeId);
            for (const auto &key : sdk) if (!key.isEmpty()) sdkCurrent << key.toString(QKeySequence::PortableText);
            ++compared;
            if (!ShortcutsTypes::sameKeys(action.current, sdk)) mismatches << action.id.action;
            actions << QJsonObject{{QStringLiteral("component"), action.id.component}, {QStringLiteral("context"), action.id.context},
                {QStringLiteral("action"), action.id.action}, {QStringLiteral("componentLabel"), action.componentFriendly},
                {QStringLiteral("contextLabel"), action.contextFriendly}, {QStringLiteral("label"), action.friendlyName},
                {QStringLiteral("current"), current}, {QStringLiteral("defaults"), defaults}, {QStringLiteral("sdkCurrent"), sdkCurrent},
                {QStringLiteral("editable"), action.available}};
        }
        const QString limitation = applicationOverridePresent
            ? QStringLiteral("The probe has an application-specific configuration source. SDK comparison may include application overrides; it does not establish equality with the global standard bindings alone.") : QString();
        QTextStream(stdout) << QJsonDocument(QJsonObject{{QStringLiteral("success"), success}, {QStringLiteral("owner"), state.owner},
            {QStringLiteral("available"), state.available}, {QStringLiteral("writable"), state.writable}, {QStringLiteral("reason"), state.reason},
            {QStringLiteral("error"), error}, {QStringLiteral("revision"), QString::fromLatin1(state.revision.toHex())},
            {QStringLiteral("configRevision"), QString::fromLatin1(state.configRevision.toHex())}, {QStringLiteral("actions"), actions},
            {QStringLiteral("sdkCompared"), compared}, {QStringLiteral("sdkMismatchCount"), mismatches.size()},
            {QStringLiteral("sdkMismatchIds"), mismatches}, {QStringLiteral("applicationConfigName"), nativeName},
            {QStringLiteral("applicationConfigSources"), applicationSources}, {QStringLiteral("applicationOverridePresent"), applicationOverridePresent},
            {QStringLiteral("sdkComparisonLimitation"), limitation}}).toJson(QJsonDocument::Indented);
        app.exit(success && (applicationOverridePresent || mismatches.isEmpty()) ? 0 : 1);
    });
    QTimer::singleShot(0, &backend, [&] { backend.readCurrent(1); });
    QTimer::singleShot(35000, &app, [&] { app.exit(2); });
    return app.exec();
}
