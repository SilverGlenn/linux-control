#pragma once

#include <QDialog>
#include <QPointer>
#include "settings/ShortcutScheme.h"
#include "PageId.h"

class ShortcutsSettings;
class CustomShortcutsSettings;
class ShortcutImportCoordinator;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;

// Reviewing a scheme never writes preferences. Import commits only drafts;
// the normal settings pages remain responsible for Apply and Revert.
class ShortcutSchemeDialog final : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Import, Export };
    Q_ENUM(Mode)
    ShortcutSchemeDialog(ShortcutsSettings *globals, ShortcutsSettings *standards, Mode mode, QWidget *parent = nullptr);
    void setBuiltInSchemes(const QList<ShortcutScheme::BuiltIn> &schemes);
    void setCustomSession(CustomShortcutsSettings *session);
    // Import-only, non-owning; its injected sessions become authoritative.
    // Closing suspends the review. Discard clears source/choices, never drafts.
    // A destroyed coordinator blocks this view instead of enabling a fallback.
    void setImportCoordinator(ShortcutImportCoordinator *coordinator);
    ShortcutImportCoordinator *importCoordinator() const;
    bool mapCommand(const QString &source, const QString &target);
    bool loadData(const QByteArray &bytes);
    bool loadFile(const QString &path);
    bool exportSelected(const QString &path, bool overwriteConfirmed = false);
    QString errorText() const { return m_error; }
    ShortcutsSettings *globalSession() const;
    ShortcutsSettings *standardSession() const;
public slots:
    bool stageSelected();
    bool stageCommandDefinitions();
    void reviewAgain();
signals:
    void navigationRequested(PageId page);
private:
    struct Row {
        ShortcutScheme::Scope scope;
        ShortcutActionId id;
        QList<QKeySequence> keys;
        bool matched = false;
        bool selectable = false;
        QTreeWidgetItem *item = nullptr;
    };
    void review(const ShortcutScheme::ReadResult &result);
    void populate(bool preserveChecks = false);
    void updateReview();
    bool baselineCurrent() const;
    void selectedChanges(ShortcutChanges &globals, ShortcutChanges &standards) const;
    void chooseFile();
    void chooseExport();
    void chooseCommandMapping();
    void pinBaseline();
    void syncFromCoordinator();
    void syncChoicesToCoordinator();
    QList<ShortcutScheme::Command> selectedCommands() const;
    QPointer<ShortcutsSettings> m_globals, m_standards;
    QPointer<CustomShortcutsSettings> m_custom;
    QPointer<ShortcutImportCoordinator> m_importCoordinator;
    Mode m_mode;
    ShortcutScheme::ReadResult m_read;
    ShortcutScheme::ImportPlan m_plan;
    QList<Row> m_rows;
    struct CommandRow { ShortcutScheme::Command definition; bool selectable = false; QTreeWidgetItem *item = nullptr; };
    QList<CommandRow> m_commands;
    QMap<QString, QString> m_migrations;
    QByteArray m_globalApplied, m_globalDraft, m_standardApplied, m_standardDraft;
    QByteArray m_customApplied, m_customDraft;
    QString m_error;
    bool m_updating = false, m_loaded = false;
    bool m_coordinatorRequired = false, m_syncingChoices = false;
    quint64 m_sessionRevision = 0, m_reviewRevision = 0;
    bool m_reviewHadGlobals = false, m_reviewHadStandards = false, m_reviewHadCustom = false;
    QTreeWidget *m_tree;
    QLineEdit *m_path, *m_name;
    QComboBox *m_builtIns;
    QCheckBox *m_reassign;
    QLabel *m_report, *m_summary, *m_workflow, *m_intro;
    QPushButton *m_commit, *m_browse, *m_reviewFile, *m_reviewBuiltIn;
    QPushButton *m_stageCommands, *m_mapCommand, *m_updateReview;
    QPushButton *m_continueCustom, *m_refreshBindings, *m_discardReview, *m_close;
};
