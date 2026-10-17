#pragma once

#include <QObject>
#include <QPointer>
#include "ShortcutScheme.h"
#include "CustomShortcutsSettings.h"

class ShortcutsSettings;

// App-owned import review. Only the existing settings sessions perform Apply;
// this object retains source/choices and coordinates verified dependency reads.
class ShortcutImportCoordinator final : public QObject {
    Q_OBJECT
public:
    enum class Phase { Empty, Review, AwaitCustomApply, AwaitGlobalRefresh, RefreshingGlobals, ReviewBindings, Staged, Complete };
    Q_ENUM(Phase)
    static ShortcutImportCoordinator *instance();
    ShortcutImportCoordinator(ShortcutsSettings *globals, ShortcutsSettings *standards,
                             CustomShortcutsSettings *custom, QObject *parent = nullptr);
    ShortcutsSettings *globalSession() const;
    ShortcutsSettings *standardSession() const;
    CustomShortcutsSettings *customSession() const;
    bool hasDocument() const { return m_read.success; }
    bool hasPendingWork() const;
    QByteArray sourceBytes() const { return m_source; }
    QByteArray sourceHash() const { return m_sourceHash; }
    QString sourcePath() const { return m_path; }
    const ShortcutScheme::ReadResult &readResult() const { return m_read; }
    const ShortcutScheme::ImportPlan &plan() const { return m_plan; }
    const QMap<QString, QString> &migrations() const { return m_migrations; }
    quint64 generation() const { return m_generation; }
    Phase phase() const;
    QString statusText() const;
    QString errorText() const { return m_error; }
    bool reviewCurrent() const;
    bool bindingSelected(ShortcutScheme::Scope scope, const ShortcutActionId &resolvedId) const;
    bool commandSelected(const QString &resolvedComponent) const;
    bool reassignConflicts() const { return m_reassign; }
    bool canStageBindings(QString *reason = nullptr) const;
    bool canStageDefinitions(QString *reason = nullptr) const;
    bool canRefreshGlobals(QString *reason = nullptr) const;
    bool loadData(const QByteArray &bytes, const QString &path = {});
    bool loadFile(const QString &path);
    bool mapCommand(const QString &source, const QString &target);
    bool setBindingSelected(ShortcutScheme::Scope scope, const ShortcutActionId &resolvedId, bool selected);
    bool setCommandSelected(const QString &resolvedComponent, bool selected);
    void setReassignConflicts(bool reassign);
public slots:
    bool review();
    bool stageDefinitions();
    bool stageBindings();
    bool refreshGlobals();
    void discard(); // Discards only this review, never any settings draft.
signals:
    void changed();
private:
    ShortcutActionId sourceId(ShortcutScheme::Scope scope, const ShortcutActionId &resolvedId) const;
    QString sourceComponent(const QString &resolvedComponent) const;
    QList<CustomShortcutCommand> selectedDefinitions() const;
    bool definitionsApplied(const QList<CustomShortcutCommand> &definitions, const QString &owner = {}) const;
    void selectedChanges(ShortcutChanges &globals, ShortcutChanges &standards, QString &error) const;
    bool bindingsApplied() const;
    void pinReview();
    void resetDependencies();
    void sessionChanged(bool global);
    void customApplied(bool success);
    void scheduleRefresh();
    void finishRefresh();
    bool reject(const QString &error);
    QPointer<ShortcutsSettings> m_globals, m_standards;
    QPointer<CustomShortcutsSettings> m_custom;
    QByteArray m_source, m_sourceHash;
    QString m_path, m_error;
    ShortcutScheme::ReadResult m_read;
    ShortcutScheme::ImportPlan m_plan;
    QMap<QString, QString> m_migrations;
    QMap<ShortcutActionId, bool> m_globalChecks;
    QMap<QString, bool> m_standardChecks, m_commandChecks;
    QList<CustomShortcutCommand> m_dependencies;
    QString m_dependencyOwner, m_refreshOwner;
    quint64 m_generation = 0, m_epoch = 0, m_reviewEpoch = 0, m_refreshGeneration = 0;
    QByteArray m_globalApplied, m_globalDraft, m_standardApplied, m_standardDraft, m_customApplied, m_customDraft;
    bool m_hadGlobals = false, m_hadStandards = false, m_hadCustom = false, m_reviewValid = false;
    bool m_reassign = false, m_definitionsRequested = false, m_waitCustomApply = false;
    bool m_dependenciesApplied = false, m_needsRefresh = false, m_refreshing = false, m_refreshScheduled = false;
    bool m_bindingsStaged = false;
};
