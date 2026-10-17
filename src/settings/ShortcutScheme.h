#pragma once
#include "ShortcutsTypes.h"

namespace ShortcutScheme {
enum class Scope { Global, Standard };
enum class Severity { Warning, Error };
struct Issue {
    Severity severity = Severity::Warning;
    QString code, message;
    ShortcutActionId id;
};
struct Binding {
    Scope scope = Scope::Global;
    ShortcutActionId id;
    QList<QKeySequence> keys;
};
struct Command { QString component, name, exec; };
struct Document {
    QString name;
    QList<Binding> bindings;
    QList<Command> commands;
};
struct ReadResult {
    bool success = false;
    Document document;
    QList<Issue> issues;
    QString error;
};
struct ImportPlan {
    bool valid = false;
    ShortcutChanges globals, standards;
    QList<Binding> unresolved;
    QList<Command> commands;
    QMap<QString,QString> migrations;
    QList<Issue> issues;
    QString error;
};
struct Selection {
    bool globals = true, standards = true;
    // Empty action/component lists select the entire enabled scope. Otherwise
    // the union of explicitly selected actions and components is exported.
    QList<ShortcutActionId> globalActions, standardActions;
    QStringList globalComponents, standardComponents;
};
struct ExportResult { bool success = false; QString error; QList<Issue> issues; };
struct BuiltIn { QString name, path; QList<Issue> issues; };
ReadResult parse(const QByteArray &bytes);
ReadResult readFile(const QString &path);
ImportPlan planImport(const Document &document, const ShortcutsSnapshot &globals,
                      const ShortcutsSnapshot &standards, const QMap<QString,QString> &commandMigrations = {});
ExportResult exportFile(const QString &path, const QString &name, const ShortcutsSnapshot &globals,
                        const ShortcutsSnapshot &standards, const Selection &selection = {},
                        const QList<Command> &commands = {});
QList<BuiltIn> builtInSchemes(const QStringList &dataRoots = {});
}
