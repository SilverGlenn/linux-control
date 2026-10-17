#pragma once

#include <QWidget>
#include "PageId.h"
class QScrollArea;
class QLabel;
class QPushButton;
class NetworkSettings;
class SettingsSession;

class NetworkSharingPage : public QWidget {
    Q_OBJECT
public:
    explicit NetworkSharingPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    NetworkSharingPage(QScrollArea *sidebar, NetworkSettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
signals:
    void navigationRequested(PageId page);
private:
    void updateState();
    NetworkSettings *m_session;
    QLabel *m_network;
    QLabel *m_connectivity;
    QLabel *m_status;
    QLabel *m_active;
    QLabel *m_confirmation;
    QPushButton *m_keep;
    QPushButton *m_undo;
};
