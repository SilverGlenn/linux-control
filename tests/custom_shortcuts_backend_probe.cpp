#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include "platform/shortcuts/CustomShortcutsBackend.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // Deliberately read-only: no writeChanges, command execution, registration,
    // cleanup, cache builder, service reload, or KCM launch exists in this probe.
    KdeCustomShortcutsBackend backend;
    QObject::connect(&backend, &CustomShortcutsBackend::readFinished, &app,
        [&](quint64, bool success, const CustomShortcutsSnapshot &state, const QString &error) {
        QJsonArray applications, entries, actions;
        QJsonObject contexts;
        for (auto it = state.contexts.cbegin(); it != state.contexts.cend(); ++it) contexts.insert(it.key(), QJsonArray::fromStringList(it.value()));
        for (const auto &application : state.applications)
            applications << QJsonObject{{QStringLiteral("id"), application.id}, {QStringLiteral("name"), application.name},
                {QStringLiteral("path"), application.path}, {QStringLiteral("command"), application.command},
                {QStringLiteral("nativeActionCount"), application.defaults.size()}};
        for (const auto &entry : state.entries)
            entries << QJsonObject{{QStringLiteral("component"), entry.component}, {QStringLiteral("name"), entry.name},
                {QStringLiteral("path"), entry.path}, {QStringLiteral("command"), entry.command},
                {QStringLiteral("registered"), entry.registered}, {QStringLiteral("editable"), entry.editable},
                {QStringLiteral("removable"), entry.removable}, {QStringLiteral("reason"), entry.reason}};
        for (const auto &action : state.actions) {
            QJsonArray current, defaults;
            for (const auto &key : action.current) current << key.toString(QKeySequence::PortableText);
            for (const auto &key : action.defaults) defaults << key.toString(QKeySequence::PortableText);
            actions << QJsonObject{{QStringLiteral("component"), action.id.component}, {QStringLiteral("context"), action.id.context},
                {QStringLiteral("action"), action.id.action}, {QStringLiteral("current"), current}, {QStringLiteral("defaults"), defaults}};
        }
        QTextStream(stdout) << QJsonDocument(QJsonObject{{QStringLiteral("success"), success}, {QStringLiteral("owner"), state.owner},
            {QStringLiteral("available"), state.available}, {QStringLiteral("writable"), state.writable},
            {QStringLiteral("interfaceSignature"), state.interfaceSignature}, {QStringLiteral("reason"), state.reason},
            {QStringLiteral("error"), error}, {QStringLiteral("revision"), QString::fromLatin1(state.revision.toHex())},
            {QStringLiteral("configRevision"), QString::fromLatin1(state.configRevision.toHex())},
            {QStringLiteral("inventoryRevision"), QString::fromLatin1(state.inventoryRevision.toHex())},
            {QStringLiteral("applications"), applications}, {QStringLiteral("entries"), entries}, {QStringLiteral("actions"), actions}, {QStringLiteral("contexts"), contexts}}).toJson(QJsonDocument::Indented);
        app.exit(success ? 0 : 1);
    });
    QTimer::singleShot(0, &backend, [&] { backend.readCurrent(1); });
    QTimer::singleShot(35000, &app, [&] { app.exit(2); });
    return app.exec();
}
