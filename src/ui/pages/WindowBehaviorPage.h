#pragma once

#include <QWidget>
#include "PageId.h"

class QScrollArea;
class QComboBox;
class QLabel;
class QPushButton;
class AppearanceSettings;
class SettingsSession;

class WindowBehaviorPage final : public QWidget {
    Q_OBJECT
public:
    explicit WindowBehaviorPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    WindowBehaviorPage(QScrollArea *sidebar, AppearanceSettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
private:
    void updateState();
    AppearanceSettings *m_session;
    QComboBox *m_action;
    QLabel *m_status;
    QLabel *m_message;
    QPushButton *m_apply;
    QPushButton *m_revert;
};
