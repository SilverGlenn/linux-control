#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QString>
#include "PageId.h"

class QScrollArea;
class QVBoxLayout;

// The "Linux Firewall" detail page, a Linux-flavoured take on the Windows 7
// "Help protect your computer with Windows Firewall" screen.
//
// This is a read-only view of selected UFW configuration files. It does not
// verify that UFW is running or that the configured rules are enforced.
class FirewallPage : public QWidget {
    Q_OBJECT

public:
    explicit FirewallPage(QScrollArea *sidebar, QWidget *parent = nullptr);

    // Left-nav entries shown by MainWindow's subpage sidebar.
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();

private:
    // Live firewall facts, read once in the constructor from the ufw config
    // files that KDE's firewall module also consults.
    struct FwInfo {
        bool    enabledKnown = false;
        bool    enabled = false;
        QString inputPolicy;           // empty unless a recognized policy is configured
        QString outputPolicy;
        QString logLevel;              // configured logging level, not notification behavior
        bool    ruleCountKnown = false;
        int     ruleCount = 0;
        bool    routeKnown = false;
        bool    netConnected = false;  // IPv4 default route exists
    };

    static FwInfo gatherInfo();

    // Builds one collapsible network-location panel (the green-striped boxes in
    // the reference). `expanded` panels show the firewall status grid beneath
    // the header; collapsed ones show only the header strip.
    QWidget *buildLocationPanel(const QString &title,
                                const QString &connState,
                                bool expanded,
                                const FwInfo &info);
};
