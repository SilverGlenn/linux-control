#pragma once

#include <QWidget>
#include <QStringList>
#include "PageId.h"
#include <QSet>
#include <QHash>
#include <QDateTime>
#include "../../operations/UpdateService.h"

class QScrollArea;
class QLabel;
class QProgressBar;
class QPushButton;
class QFrame;
class QWidget;
class QVBoxLayout;
class QStackedWidget;
class QTreeWidget;

class LinuxUpdatePage : public QWidget {
    Q_OBJECT

public:
    explicit LinuxUpdatePage(QScrollArea *sidebar, QWidget *parent = nullptr);

    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();

public slots:
    void checkForUpdates();

    // Switch between the update-status view and the in-window
    // "Select updates to install" view (driven by MainWindow navigation).
    void showStatusView();
    void showSelectView();

signals:
    void navigateHomeRequested();
    // Emitted when the user clicks an "updates available" hyperlink; MainWindow
    // navigates to the in-window select-updates page in response.
    void selectUpdatesRequested();
    // Emitted by the select-updates page's OK/Cancel; MainWindow navigates back.
    void selectViewClosed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void openUpdateList();
    void installUpdates();
    void syncFromService();

private:
    using PkgInfo = UpdatePackage;

    void setIdleState();
    void setCheckingState();
    void setNoUpdatesState();
    void setCheckErrorState(const QString &message);
    void setUpdatesAvailableState();
    void setDownloadingState();
    void setInstallingPhaseState();
    void setDoneState(bool ok);
    void resetStatusWidgets();   // hide every optional status-box widget
    void applyStatusBorder(const QString &leftColor, const QString &bg = "#FFFFFF");
    void updateInfoRows();
    static QString formatStamp(const QDateTime &dt);
    void setLinkLabel(QLabel *label, const QString &text, bool underline);

    // Select-updates page
    QWidget *buildSelectView();      // rebuilds the page from the current m_pkgs
    void     populateSelectTable();  // fills the table for the active filter
    void     updateSelectCount();    // refreshes the footer "Total selected" text
    void     setSelectFilter(int filter);
    void     applySelection();       // pushes the checked set back to the status view

    // Status box
    QFrame       *m_statusBox      = nullptr;
    QLabel       *m_iconLabel      = nullptr;
    QLabel       *m_titleLabel     = nullptr;
    QLabel       *m_subLabel       = nullptr;   // hyperlink in updates-available state
    QLabel       *m_subLabel2      = nullptr;   // second hyperlink line (optional updates)
    QProgressBar *m_progressBar    = nullptr;
    QLabel       *m_progressText   = nullptr;

    // Right column (only visible when updates available)
    QWidget      *m_rightWidget    = nullptr;
    QLabel       *m_selectedLabel  = nullptr;
    QPushButton  *m_installBtn     = nullptr;

    // Subtitle row widget (hidden entirely in states with no subtitle)
    QWidget      *m_subRowWidget   = nullptr;

    // Subtitle row separator (visible only in updates-available state)
    QFrame       *m_subSep         = nullptr;

    // Info rows
    QLabel       *m_lastCheckVal   = nullptr;
    QLabel       *m_lastInstallVal = nullptr;

    // Top-level stack: index 0 = status view, index 1 = select-updates view.
    QStackedWidget *m_stack        = nullptr;
    QWidget        *m_selectView   = nullptr;

    // Select-updates page widgets (valid only while that view exists).
    QTreeWidget  *m_selTree        = nullptr;
    QLabel       *m_selGroupLabel  = nullptr;
    QLabel       *m_selCountLabel  = nullptr;
    QLabel       *m_selDetailTitle = nullptr;
    QLabel       *m_selDetailBody  = nullptr;
    QLabel       *m_selDetailNote  = nullptr;
    QPushButton  *m_selImpItem     = nullptr;   // "Important (N)" sidebar entry
    QPushButton  *m_selOptItem     = nullptr;   // "Optional (N)" sidebar entry

    QList<PkgInfo>  m_selPkgs;        // sorted important-first for the select view
    QSet<QString>   m_selChecked;     // names currently ticked in the select view
    int             m_selFilter      = 0;   // 0 = Important, 1 = Optional
    bool            m_selUpdating    = false; // guards itemChanged during refills
    QHash<QString, QString> m_pkgSizes;     // package name -> human download size

    QList<PkgInfo>  m_pkgs;
    QStringList     m_selectedPkgNames;
    int             m_selectedCount = 0;
    QDateTime       m_lastCheck;
    QDateTime       m_lastInstall;
    bool            m_everInstalled  = false;
    bool            m_lastInstallOk  = false;
    QString         m_aurCheckWarning;
};
