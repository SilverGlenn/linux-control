#pragma once
#include <QWidget>
#include "PageId.h"
class QScrollArea;
class QTreeWidget;
class QComboBox;
class QPlainTextEdit;
class QCheckBox;
class QLabel;
class QPushButton;
class QTabWidget;
class NetworkSettings;
class SettingsSession;
struct NetworkIpConfig;

class NetworkConnectionsPage : public QWidget {
    Q_OBJECT
public:
    explicit NetworkConnectionsPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    NetworkConnectionsPage(QScrollArea *sidebar, NetworkSettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
signals:
    void navigationRequested(PageId page);
private:
    struct IpFields {
        QComboBox *method;
        QPlainTextEdit *addresses;
        QPlainTextEdit *gateway;
        QPlainTextEdit *dns;
        QCheckBox *ignoreAutoDns;
    };
    QWidget *buildIpFields(IpFields &fields, const QString &prefix, const QString &family);
    void updateState();
    void stageForm();
    void connectWifi();
    void activateProfile();
    QString selectedDevice() const;
    NetworkSettings *m_session;
    bool m_syncing = false;
    bool m_staging = false;
    QTreeWidget *m_devices;
    QTreeWidget *m_accessPoints;
    QTreeWidget *m_profiles;
    QComboBox *m_wifiDevice;
    QComboBox *m_profileDevice;
    QPlainTextEdit *m_name;
    QCheckBox *m_autoconnect;
    QWidget *m_form;
    IpFields m_ipv4;
    IpFields m_ipv6;
    QLabel *m_deviceDetails;
    QLabel *m_status;
    QLabel *m_support;
    QLabel *m_profileSupport;
    QLabel *m_confirmation;
    QPushButton *m_scan;
    QPushButton *m_wifiConnect;
    QPushButton *m_profileConnect;
    QPushButton *m_disconnect;
    QPushButton *m_refresh;
    QPushButton *m_apply;
    QPushButton *m_revert;
    QPushButton *m_keep;
    QPushButton *m_undo;
};
