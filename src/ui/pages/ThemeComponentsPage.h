#pragma once
#include "PageId.h"
#include <QWidget>
#include <QHash>
class QScrollArea;
class QLabel;
class QPushButton;
class SettingsSession;
class AeroThemeSettings;
class ThemeComponentsPage final : public QWidget {
    Q_OBJECT
public:
    explicit ThemeComponentsPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    ThemeComponentsPage(QScrollArea *sidebar, AeroThemeSettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
signals:
    void navigationRequested(PageId page);
private:
    void updateState();
    AeroThemeSettings *m_session;
    QHash<QString, QLabel *> m_components;
    QLabel *m_message, *m_review;
    QPushButton *m_restore, *m_apply, *m_revert;
};
