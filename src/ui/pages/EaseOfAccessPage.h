#pragma once

#include <QWidget>
#include <QStringList>
#include "PageId.h"

class QScrollArea;
class QVBoxLayout;

// The "Ease of Access Center" detail page, a Control-Panel rendering of the
// Windows 7 Ease of Access Center.
//
// No standalone accessibility apps (magnifier, narrator, on-screen keyboard) are
// assumed to be installed, so, like the real Control Panel, whose entries open
// sub-dialogs, supported tools open their matching KDE settings module.
// Appearance links use the native Aero editor; broad theme editors are disabled.
class EaseOfAccessPage : public QWidget {
    Q_OBJECT

public:
    explicit EaseOfAccessPage(QScrollArea *sidebar, QWidget *parent = nullptr);

    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();

signals:
    void navigationRequested(PageId page);

private:
    // Append an icon + blue link that opens `cmd` when clicked.
    void addSettingLink(QVBoxLayout *into, const QString &iconName,
                        const QString &text, const QStringList &cmd);
};
