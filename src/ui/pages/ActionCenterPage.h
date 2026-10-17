#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include "PageId.h"

class QScrollArea;
class QVBoxLayout;

// The "Action Center" detail page, a Linux-flavoured take on the Windows 7
// "Review recent messages and resolve problems" screen.
//
// Like the Windows original it groups status into two collapsible sections,
// Security and Maintenance, each a stack of "item / state / description" rows
// with the state right-aligned. Rows report only what the app can verify; other
// Windows-era concepts are marked unmonitored rather than assigned plausible
// looking status values.
class ActionCenterPage : public QWidget {
    Q_OBJECT

public:
    explicit ActionCenterPage(QScrollArea *sidebar, QWidget *parent = nullptr);

    // Left-nav entries shown by MainWindow's subpage sidebar.
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();

private:
    // Facts backing the Security rows, gathered once in the constructor.
    struct AcInfo {
        QString firewallState;
        QString firewallDescription;
        bool    avPresent = false;    // scanner executable exists; runtime state is unverified
        QString avName;
        bool    polkitKnown = false;
        bool    polkitAvailable = false; // authority registered; policy coverage unverified
    };

    static AcInfo gatherInfo();

    // Builds one collapsible section (Security / Maintenance). `rows` is the
    // pre-populated body; the returned frame carries the clickable blue header
    // with the chevron that shows/hides that body.
    QWidget *buildSection(const QString &title, bool expanded, QWidget *rows);

    // A single "item, state, description" row inside a section body. `link`,
    // when non-empty, adds a blue task link under the description.
    QWidget *buildStatusRow(const QString &item,
                            const QString &state,
                            const QString &description,
                            const QString &link = QString());

    // The bottom "If you don't see your problem listed" icon + text pair.
    QWidget *buildBottomTask(std::initializer_list<const char *> iconNames,
                             const QString &title,
                             const QString &description);
};
