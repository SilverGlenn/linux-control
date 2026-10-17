#pragma once
#include <QWidget>
#include "PageId.h"
#include "settings/DefaultAppsSettings.h"
#include <QMap>
class QScrollArea;
class QLabel;
class QPushButton;
class QComboBox;
class QLineEdit;
class QTreeWidget;
class DefaultAppsPage final : public QWidget {
    Q_OBJECT
public:
    explicit DefaultAppsPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    DefaultAppsPage(QScrollArea *sidebar, DefaultAppsSettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
private:
    void updateState();
    void updateAssociations();
    void updateMimeChoices();
    DefaultAppsSettings *m_session;
    QMap<DefaultAppRole, QComboBox *> m_roles;
    QLineEdit *m_search;
    QTreeWidget *m_types;
    QComboBox *m_mimeApps;
    QPushButton *m_change, *m_apply, *m_revert, *m_defaults;
    QLabel *m_status, *m_message, *m_mimeDescription;
    QByteArray m_inventoryRevision;
    bool m_updating = false;
};
