#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include "PageId.h"
#include <QList>
#include <QDate>
#include <QSoundEffect>
#include "packages/ProgramsService.h"

class QScrollArea;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;

// The "Programs and Features" detail page, a Linux-flavoured take on the
// Windows 7 "Uninstall or change a program" screen.
//
// The "installed programs" are the explicitly-installed packages reported by
// `pacman -Qei`; each block gives the name, version, install date and
// installed size, which map onto the Name / Version / Installed On / Size
// columns. The Publisher column shows the software's actual developer
// (AppStream data) or, for AUR packages, its AUR maintainer, see
// applyPublishers().
class ProgramsFeaturesPage : public QWidget {
    Q_OBJECT

public:
    explicit ProgramsFeaturesPage(QScrollArea *sidebar, QWidget *parent = nullptr,
                                  ProgramsService *service = nullptr);

    // Left-nav entries shown by MainWindow's subpage sidebar.
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();

private:
    using ProgramInfo = ProgramsService::ProgramInfo;
    using LoadResult = ProgramsService::LoadResult;
    void startLoad();
    void refreshFromService();

    // Fill the tree from gathered data, replacing the "Searching..." placeholder.
    void populate(const LoadResult &result);

    // Double-clicking a program opens a Windows-style Uninstall/Reinstall/Cancel
    // dialog; the chosen action shells out to pacman/yay.
    void onProgramActivated(QTreeWidgetItem *item, int column);
    void runPackageAction(const QString &pkg, const QString &display, bool reinstall);

    QTreeWidget *m_tree     = nullptr;
    QLabel      *m_countLbl = nullptr;
    QLabel      *m_totalLbl = nullptr;
    ProgramsService *m_service = nullptr;
    QLabel *m_actionStatus = nullptr;
    int m_inventoryRevision = -1;
    bool m_showingLoading = false;
    QSoundEffect *m_dialogSound = nullptr; // created only when a confirmation needs sound
};
