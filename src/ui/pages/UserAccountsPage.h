#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include "PageId.h"

class QScrollArea;
class QLineEdit;
class QLabel;
class QPushButton;
class AccountSettings;
class SettingsSession;

// Native current-user display-name editor; its persistent session owns authorization and readback.
class UserAccountsPage : public QWidget {
    Q_OBJECT

public:
    explicit UserAccountsPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    UserAccountsPage(QScrollArea *sidebar, AccountSettings *session, QWidget *parent = nullptr);
    SettingsSession *settingsSession() const;

    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();

private:
    struct Account {
        QString userName;      // login name
        QString fullName;      // GECOS display name (falls back to login name)
        QString accountType;   // "Administrator" or "Standard user"
        QString picturePath;   // avatar image path, empty if none found
    };
    static Account gatherAccount();
    void updateState();
    AccountSettings *m_session;
    QLineEdit *m_name;
    QLabel *m_status;
    QLabel *m_summaryName;
    QLabel *m_identity;
    QPushButton *m_apply;
    QPushButton *m_revert;
    QPushButton *m_cancel;
};
