#pragma once

#include <QMainWindow>
#include <QStringList>
#include <QHash>
#include <QPointer>
#include <QSoundEffect>
#include <QGraphicsOpacityEffect>
#include <functional>
#include "PageId.h"
#include "ActionId.h"

class QScrollArea;
class QLineEdit;
class QToolButton;
class QPushButton;
class QLabel;
class QHBoxLayout;
class QVBoxLayout;
class CategoryWidget;
class LinuxUpdatePage;
class SettingsNavigationGuard;
class SettingsSession;
class UserFontService;
class PackageSimulationService;
class AudioSettings;
class QCloseEvent;
class QFrame;
class QTimer;
class LinkLabel;
class ShortcutImportCoordinator;

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class MainWindowLifecycleTest;
    friend class ShellNavigationTest;
    friend class KdeIntegrationTest;

public:
    explicit MainWindow(QWidget *parent = nullptr, bool enableSounds = true);
    void setShortcutImportCoordinator(ShortcutImportCoordinator *coordinator);
    ShortcutImportCoordinator *shortcutImportCoordinator() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    // The shared chrome of a left-nav pane: a width-clipped scroll area, the
    // grey navPane frame, and the text layout (already seeded with the
    // "Control Panel Home" link) that callers fill with their own entries.
    struct Sidebar {
        QScrollArea *clip;
        QWidget     *textWrap;
        QVBoxLayout *navV;
    };
    Sidebar buildSidebarShell(int initialWidth);

    void buildCrumbBar();
    QWidget *buildHomePage();
    QWidget *buildCategoryPage(const QString &category);
    QScrollArea *buildNavSidebar(const QString &currentCategory);
    QScrollArea *buildSubpageSidebar(const QList<SidebarLink> &links,
                                     const QList<SidebarLink> &seeAlso = {});

    // Point a freshly built sidebar label at the destination its link declares
    // (another page, home, an external command, or an in-app applet).
    void registerLinkTarget(LinkLabel *label, const LinkTarget &target);
    void registerActionLink(LinkLabel *label, ActionId action);
    void activateAction(ActionId action);
    void activateTarget(const LinkTarget &target);

    void setCrumbTrail(const QStringList &trail);
    void navigateHome();
    void navigateTo(const QString &path);
    void navigateSearch(const QString &query);
    QWidget *buildSearchPage(const QString &query);
    enum class ViewMode { Category, LargeIcons, SmallIcons };
    void setViewMode(ViewMode mode);
    void syncSearchBox();
    void focusDestination();

    // Open an in-app applet dialog (e.g. "datetime", "datetime:additional"),
    // the Windows-style modal popup for a Control Panel item.
    void openApplet(const QString &id);

    // History-aware rendering. An empty string represents the home page.
    void pushHistory(const QString &entry);
    void showEntry(const QString &entry);
    void goBack();
    void goForward();
    void updateNavButtons();
    void requestNavigation(std::function<void()> action);
    void requestCloseApproval();
    SettingsSession *currentSettingsSession() const;
    void refreshJobStrip();
    void registerSettingsSession(SettingsSession *session, const QString &title);
    void registerFontService(UserFontService *service);
    void registerPackageSimulation(PackageSimulationService *service);
    void registerAudioSettings(AudioSettings *settings);
    void openShortcutImport();
    void playNavigationSound();
    bool hasBackgroundWork() const;

signals:
    void backgroundStatusChanged();

private:
    struct WatchedSession { QPointer<SettingsSession> session; QString title; };
    QList<WatchedSession> m_settingsSessions;
    QPointer<UserFontService> m_fontService;
    QPointer<PackageSimulationService> m_packageSimulation;
    QPointer<AudioSettings> m_audioSettings;
    QPointer<ShortcutImportCoordinator> m_importCoordinator;
    QString m_settingsResult;

    QWidget     *m_crumbBar;
    QHBoxLayout *m_pathLayout = nullptr;
    QLineEdit   *m_searchBox;
    QScrollArea *m_scroll;
    SettingsNavigationGuard *m_navigationGuard = nullptr;
    QFrame *m_jobStrip = nullptr;
    QLabel *m_jobLabel = nullptr;
    QPushButton *m_jobDetails = nullptr;
    QPushButton *m_resumeImport = nullptr;

    QPushButton *m_backBtn = nullptr;
    QPushButton *m_forwardBtn = nullptr;

    QStringList m_history;
    int         m_historyIndex = -1;

    ViewMode m_viewMode = ViewMode::Category;
    QTimer *m_searchTimer = nullptr;
    QString m_visibleSearch;

    // "Control Panel" crumb; a QLabel (not a button) so AeroQt's glass-bar
    // QAbstractButton texture never applies. Clicks go to the home view.
    QLabel *m_homeCrumb = nullptr;

    QSoundEffect *m_navSound = nullptr;
    bool m_soundsEnabled;

    // Opacity effect on the current category sidebar's text wrap; used to fade
    // out the text before navigating to a subcategory.
    QGraphicsOpacityEffect *m_sidebarTextEffect = nullptr;

    LinuxUpdatePage *m_updatePage = nullptr;
};
