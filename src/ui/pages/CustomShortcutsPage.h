#pragma once
#include <QWidget>
#include "PageId.h"

class CustomShortcutsSettings;
class SettingsSession;
class QScrollArea;
class QTreeWidget;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;

class CustomShortcutsPage final : public QWidget {
    Q_OBJECT
public:
    explicit CustomShortcutsPage(QScrollArea *, QWidget *parent = nullptr);
    CustomShortcutsPage(QScrollArea *, CustomShortcutsSettings *, QWidget *parent = nullptr);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
signals:
    void navigationRequested(PageId page);
private:
    void updateState();
    void updateSelection();
    void filterApplications();
    void finishEditing();
    QString selectedComponent() const;
    CustomShortcutsSettings *m_session;
    QTreeWidget *m_entries;
    QComboBox *m_applications;
    QLineEdit *m_filter, *m_name, *m_command;
    QLabel *m_details, *m_status;
    QPushButton *m_addApplication, *m_register, *m_edit, *m_remove, *m_restore, *m_assign;
    QPushButton *m_saveCommand, *m_cancelEdit, *m_refresh, *m_revert, *m_apply;
    QString m_editComponent;
    bool m_updating = false;
};
