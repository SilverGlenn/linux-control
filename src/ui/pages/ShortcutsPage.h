#pragma once

#include <QWidget>
#include <QPointer>
#include "PageId.h"
#include "settings/ShortcutsTypes.h"

class QScrollArea;
class QLineEdit;
class QTreeWidget;
class QListWidget;
class KKeySequenceWidget;
class QLabel;
class QPushButton;
class QGroupBox;
class ShortcutsSettings;
class CustomShortcutsSettings;
class ShortcutImportCoordinator;
class SettingsSession;

class ShortcutsPage final : public QWidget {
    Q_OBJECT
public:
    enum class Scope { Global, StandardApplications };
    Q_ENUM(Scope)
    explicit ShortcutsPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    ShortcutsPage(QScrollArea *sidebar, ShortcutsSettings *session, QWidget *parent = nullptr);
    ShortcutsPage(QScrollArea *sidebar, Scope scope, QWidget *parent = nullptr);
    ShortcutsPage(QScrollArea *sidebar, ShortcutsSettings *session, Scope scope, QWidget *parent = nullptr);
    SettingsSession *settingsSession() const;
    // Optional app-owned companions. Injected pages never create providers for
    // missing scopes or definitions; destroying a companion clears the link.
    void setCompanionSession(ShortcutsSettings *session);
    ShortcutsSettings *companionSession() const;
    void setCustomSession(CustomShortcutsSettings *session);
    CustomShortcutsSettings *customSession() const;
    // Non-owning; an injected page never creates an import coordinator.
    void setImportCoordinator(ShortcutImportCoordinator *coordinator);
    ShortcutImportCoordinator *importCoordinator() const;
    // Select an already registered action after discovery; never creates or invokes it.
    void focusExistingAction(const QString &component, const QString &action);
    Scope scope() const { return m_scope; }
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarLinks(Scope scope);
    static QList<SidebarLink> sidebarSeeAlso();
signals:
    void navigationRequested(PageId page);
private:
    void updateState();
    void updateSelection(bool changed = false);
    void filterTree();
    void propose(bool replace);
    void stageKeys(const QList<QKeySequence> &keys);
    void clearProposal();
    void openScheme(bool importing);
    ShortcutsSettings *m_session;
    QPointer<ShortcutsSettings> m_companion;
    QPointer<CustomShortcutsSettings> m_custom;
    QPointer<ShortcutImportCoordinator> m_importCoordinator;
    Scope m_scope;
    QLineEdit *m_search;
    QTreeWidget *m_actions;
    QListWidget *m_alternatives;
    KKeySequenceWidget *m_editor;
    QGroupBox *m_details;
    QLabel *m_defaults, *m_capability, *m_status, *m_conflicts;
    QPushButton *m_add, *m_replace, *m_remove, *m_actionDefaults, *m_allDefaults;
    QPushButton *m_reassign, *m_cancel, *m_apply, *m_revert, *m_refresh;
    QPushButton *m_import, *m_export;
    QString m_focusComponent, m_focusAction;
    ShortcutActionId m_selected, m_proposalId;
    QList<QKeySequence> m_proposal;
    bool m_hasSelection = false, m_hasProposal = false, m_updating = false;
};
