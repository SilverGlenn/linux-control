#include "MainWindow.h"
#include "CategoryWidget.h"
#include "ActionCatalog.h"
#include "CatalogView.h"
#include "LinkLabel.h"
#include "operations/UpdateService.h"
#include "operations/ControlPanelUpdateService.h"
#include "IconHelper.h"
#include "Win7Ui.h"
#include "SettingsNavigationGuard.h"
#include "settings/AeroThemeSettings.h"
#include "settings/PowerProfileService.h"
#include "operations/OperationManager.h"
#include "packages/ProgramsService.h"
#include "perf/WeiBenchmark.h"
#include "release/BuildIdentity.h"

#include <QApplication>
#include <QScrollArea>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QWidget>
#include <QFrame>
#include <QIcon>
#include <QFont>
#include <QStatusBar>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QSizeGrip>
#include <QLayoutItem>
#include <QEvent>
#include <QMouseEvent>
#include <QVariantAnimation>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QMessageBox>
#include <QCloseEvent>
#include <QFileInfo>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QSettings>
#include <QSignalBlocker>
#include <QUrl>
#include <AeroQt/navbuttons.h>
#include <AeroQt/insetwindow.h>
#include "Categories.h"
#include "Branding.h"
#include "PageId.h"
#include "PageRegistry.h"
#include "Commands.h"
#include "pages/LinuxUpdatePage.h"
#include "pages/SystemPage.h"
#include "pages/InstalledUpdatesPage.h"
#include "pages/ProgramsFeaturesPage.h"
#include "pages/NetworkSharingPage.h"
#include "pages/NetworkConnectionsPage.h"
#include "pages/InputDevicesPage.h"
#include "pages/KeyboardPage.h"
#include "pages/ShortcutsPage.h"
#include "pages/CustomShortcutsPage.h"
#include "settings/CustomShortcutsSettings.h"
#include "settings/ShortcutsSettings.h"
#include "settings/ShortcutImportCoordinator.h"
#include "dialogs/ShortcutSchemeDialog.h"
#include "pages/DefaultAppsPage.h"
#include "settings/NetworkSettings.h"
#include "pages/FirewallPage.h"
#include "pages/ActionCenterPage.h"
#include "pages/PowerOptionsPage.h"
#include "pages/DisplayPage.h"
#include "pages/NightLightPage.h"
#include "pages/NightTimePage.h"
#include "pages/ScreenLockPage.h"
#include "pages/AeroLoginPage.h"
#include "pages/WindowBehaviorPage.h"
#include "pages/DesktopBackgroundPage.h"
#include "pages/ThemeComponentsPage.h"
#include "pages/FontSettingsPage.h"
#include "settings/UserFontService.h"
#include "pages/AeroEffectsPage.h"
#include "pages/PackagePreviewPage.h"
#include "packages/PackageSimulationService.h"
#include "pages/PerformancePage.h"
#include "dialogs/SoundDialog.h"
#include "settings/AudioSettings.h"
#include "pages/PersonalizationPage.h"
#include "pages/FontsPage.h"
#include "pages/UserAccountsPage.h"
#include "pages/EaseOfAccessPage.h"
#include "pages/DevicesAndPrintersPage.h"
#include "dialogs/DateTimeDialog.h"

namespace {
const QString searchPrefix = QStringLiteral("@search/");
bool settingsWork(SettingsSession *session)
{
    if (!session) return false;
    auto *network = qobject_cast<NetworkSettings *>(session);
    return session->isBusy() || (network && network->isScanning());
}
QString searchQuery(const QString &entry)
{
    return entry.startsWith(searchPrefix)
        ? QUrl::fromPercentEncoding(entry.mid(searchPrefix.size()).toUtf8()) : QString();
}
std::function<bool()> pinAeroLoginReview(QMessageBox &box,QPushButton *apply,SettingsSession *candidate)
{
    auto *session=qobject_cast<AeroLoginSettings *>(candidate);
    if(!session)return []{return true;};
    QPointer<AeroLoginSettings> guard(session);const auto saved=session->applied(),draft=session->staged();
    QStringList review=AeroLoginTypes::reviewChanges(saved,draft);
    review<<AeroLoginTypes::loginProjection(draft)<<AeroLoginTypes::lockProjection(draft);
    review<<QObject::tr("Administrative authorization saves these files. The Aero theme stays fixed. No greeter restart or lock is performed; live adoption remains unverified.");
    if(!session->canApply())review<<session->applySupportText();
    box.setTextFormat(Qt::PlainText);box.setText(box.text()+QStringLiteral("\n\n")+review.join('\n'));
    apply->setEnabled(session->canApply());
    return [guard,saved,draft]{return guard&&guard->canApply()&&AeroLoginTypes::sameConfiguration(saved,guard->applied())&&AeroLoginTypes::sameDraft(draft,guard->staged());};
}
}

MainWindow::MainWindow(QWidget *parent, bool enableSounds)
    : QMainWindow(parent), m_soundsEnabled(enableSounds)
{
    QSettings preferences;
    const int savedMode = preferences.value(QStringLiteral("shell/viewMode"), 0).toInt();
    if (savedMode >= 0 && savedMode <= 2) m_viewMode = static_cast<ViewMode>(savedMode);
    // Title the main window explicitly rather than via applicationDisplayName,
    // which Qt would otherwise append to every dialog title ("Foo - Control
    // Panel"). With no display name set, dialogs show just their own title.
    setWindowTitle(tr("Control Panel — %1").arg(QString::fromUtf8(release::buildId())));
    setMinimumSize(700, 520);
    resize(1000, 650);

    // Hide menu bar (like real Windows 7 Control Panel)
    menuBar()->hide();

    // Crumb bar: Aero glass top inset
    buildCrumbBar();

    // Central container
    auto *central = new QWidget;
    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet("QScrollArea { background: #FFFFFF; }");
    mainLayout->addWidget(m_scroll, 1);

    m_navigationGuard = new SettingsNavigationGuard(this);
    m_jobStrip = new QFrame;
    m_jobStrip->setObjectName("backgroundJobs");
    m_jobStrip->setStyleSheet("#backgroundJobs { background: #F0F0F0; border-top: 1px solid #D9D9D9; }");
    auto *jobLayout = new QHBoxLayout(m_jobStrip);
    jobLayout->setContentsMargins(12, 6, 12, 6);
    m_jobLabel = new QLabel;
    m_jobLabel->setTextFormat(Qt::PlainText);
    m_jobLabel->setStyleSheet("color: #333333; background: transparent;");
    m_jobLabel->setWordWrap(true);
    jobLayout->addWidget(m_jobLabel, 1);
    m_resumeImport = new QPushButton(tr("Resume scheme import…"));
    m_resumeImport->setObjectName(QStringLiteral("resumeShortcutImport"));
    m_resumeImport->setAccessibleName(tr("Resume retained shortcut scheme import"));
    m_resumeImport->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+I")));
    m_resumeImport->setToolTip(tr("Resume the retained shortcut import (Ctrl+Shift+I)"));
    jobLayout->addWidget(m_resumeImport);
    connect(m_resumeImport, &QPushButton::clicked, this, &MainWindow::openShortcutImport);
    m_jobDetails = new QPushButton(QStringLiteral("Details"));
    jobLayout->addWidget(m_jobDetails);
    Win7::styleSettingsControls(m_jobStrip);
    mainLayout->addWidget(m_jobStrip);
    connect(m_navigationGuard, &SettingsNavigationGuard::pendingChanged,
            this, [this]() {
                updateNavButtons();
                m_searchBox->setReadOnly(m_navigationGuard->pending());
                if (!m_navigationGuard->pending()) {
                    // The guard emits before running an approved continuation.
                    // Restore a cancelled draft only if the user has not edited
                    // again while this deferred callback was waiting.
                    const QString draft = m_searchBox->text();
                    QTimer::singleShot(0, this, [this, draft]() {
                        if (!m_navigationGuard->pending() && m_searchBox->text() == draft)
                            syncSearchBox();
                    });
                }
            });
    connect(OperationManager::instance(), &OperationManager::changed,
            this, &MainWindow::refreshJobStrip);
    connect(ProgramsService::instance(), &ProgramsService::changed,
            this, &MainWindow::refreshJobStrip);
    connect(PowerProfileService::instance(), &PowerProfileService::stateChanged,
            this, &MainWindow::refreshJobStrip);
    connect(WeiBenchmark::instance(), &WeiBenchmark::stateChanged,
            this, &MainWindow::refreshJobStrip);
    connect(m_jobDetails, &QPushButton::clicked, this, [this]() {
        auto *dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Background work"));
        dialog->resize(650, 400);
        auto *layout = new QVBoxLayout(dialog);
        auto *text = new QPlainTextEdit;
        text->setReadOnly(true);
        layout->addWidget(text);
        auto update = [this, text]() {
            QStringList entries;
            if (m_importCoordinator && m_importCoordinator->hasDocument())
                entries << tr("Shortcut scheme import — %1\n%2").arg(m_importCoordinator->sourcePath(), m_importCoordinator->statusText());
            if (m_fontService) entries << tr("Fonts — %1\n%2").arg(
                m_fontService->busy() ? tr("Working") : tr("Idle"), m_fontService->result());
            if (m_audioSettings) entries << tr("Sound — %1\n%2\n%3").arg(
                m_audioSettings->isBusy() ? tr("Working") : tr("Idle"), m_audioSettings->errorText(), m_audioSettings->resultText());
            if (m_packageSimulation) entries << tr("Ubuntu Package Preview — %1\n%2\n%3").arg(
                m_packageSimulation->statusText(), m_packageSimulation->result().error,
                m_packageSimulation->result().rawOutput);
            for (const auto &entry : m_settingsSessions) {
                if (!entry.session) continue;
                auto *session = entry.session.data();
                entries << QStringLiteral("%1 — %2\n%3\n%4").arg(entry.title,
                    settingsWork(session) ? tr("Working") : session->isDirty() ? tr("Pending changes") : tr("Idle"),
                    session->errorText(), session->resultText());
            }
            for (auto *job : OperationManager::instance()->jobs()) {
                const QString state = job->active() ? QStringLiteral("Running")
                    : job->succeeded() ? QStringLiteral("Completed") : QStringLiteral("Failed");
                entries << QStringLiteral("%1 — %2\n%3\n%4").arg(job->title(), state,
                    job->errorString(), QString::fromUtf8(job->output()));
            }
            text->setPlainText(entries.join(QStringLiteral("\n\n")));
        };
        connect(this, &MainWindow::backgroundStatusChanged, text, update);
        update();
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(buttons);
        dialog->show();
    });
    refreshJobStrip();

    navigateHome();

    setCentralWidget(central);
    statusBar()->hide();

    // Aero glass on crumb bar: must be after setCentralWidget
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    Aero::makeInsetWindow(this, central, m_crumbBar, nullptr);

    // Intercept mouse side-button events from any child widget.
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (now && m_scroll->widget() && m_scroll->widget()->isAncestorOf(now))
            m_scroll->ensureWidgetVisible(now, 8, 8);
    });
    auto shortcut = [this](const QKeySequence &key, auto action) {
        auto *binding = new QShortcut(key, this);
        connect(binding, &QShortcut::activated, this, action);
    };
    shortcut(QKeySequence::Find, [this]() { m_searchBox->setFocus(); m_searchBox->selectAll(); });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Left), [this]() { goBack(); });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Right), [this]() { goForward(); });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Home), [this]() { navigateHome(); });
    shortcut(QKeySequence(Qt::Key_Escape), [this]() {
        if (!m_searchBox->text().isEmpty() || !m_visibleSearch.isEmpty()) {
            m_searchTimer->stop();
            navigateSearch(QString());
        }
    });
}

void MainWindow::buildCrumbBar()
{
    m_crumbBar = new QWidget;
    m_crumbBar->setFixedHeight(34);
    auto *layout = new QHBoxLayout(m_crumbBar);
    layout->setContentsMargins(4, 3, 6, 3);
    layout->setSpacing(2);

    // Aero-style back/forward nav buttons
    auto *navBtns = new Aero::NavButtons(m_crumbBar);
    m_backBtn = navBtns->back();
    m_forwardBtn = navBtns->forward();
    m_backBtn->setObjectName("navigateBack");
    m_forwardBtn->setObjectName("navigateForward");
    m_backBtn->setAccessibleName(tr("Back"));
    m_forwardBtn->setAccessibleName(tr("Forward"));
    m_backBtn->setFocusPolicy(Qt::StrongFocus);
    m_forwardBtn->setFocusPolicy(Qt::StrongFocus);
    m_backBtn->setToolTip(tr("Back (Alt+Left)"));
    m_forwardBtn->setToolTip(tr("Forward (Alt+Right)"));
    m_backBtn->setEnabled(false);
    m_forwardBtn->setEnabled(false);
    QObject::connect(m_backBtn, &QPushButton::clicked, this, &MainWindow::goBack);
    QObject::connect(m_forwardBtn, &QPushButton::clicked, this, &MainWindow::goForward);
    layout->addWidget(navBtns);
    layout->addSpacing(4);

    // Address path box: icon + crumb trail. Reuse AeroQt's glass-entry texture so
    // this box matches the search field. Scoped with #addressBox so child labels
    // are unaffected.
    auto *pathBox = new QWidget;
    pathBox->setObjectName("addressBox");
    pathBox->setStyleSheet(
        "#addressBox { border-image: url(:/AeroQt/transpbg/entry/normal.png) 4 4 4 4 repeat; border-width: 4px; }"
        "#addressBox:hover { border-image: url(:/AeroQt/transpbg/entry/hover.png) 4 4 4 4 repeat; }"
    );
    pathBox->setFixedHeight(24);
    m_pathLayout = new QHBoxLayout(pathBox);
    m_pathLayout->setContentsMargins(4, 0, 4, 0);
    m_pathLayout->setSpacing(4);

    layout->addWidget(pathBox, 1);
    layout->addSpacing(6);

    // Keep the glass entry texture and an explicit, readable text area even
    // when the desktop style changes its default line-edit metrics.
    m_searchBox = new QLineEdit;
    m_searchBox->setPlaceholderText("Search Control Panel");
    m_searchBox->setFixedWidth(190);
    m_searchBox->setFixedHeight(24);
    m_searchBox->setObjectName("controlPanelSearch");
    m_searchBox->setAccessibleName(tr("Search Control Panel"));
    m_searchBox->setToolTip(tr("Search settings (Ctrl+F)"));
    m_searchBox->setMaxLength(256);
    m_searchBox->setStyleSheet(
        "QLineEdit#controlPanelSearch { color: #000000; padding: 0px;"
        " border-image: url(:/AeroQt/transpbg/entry/normal.png) 4 4 4 4 repeat; border-width: 4px; }"
        "QLineEdit#controlPanelSearch:hover { border-image: url(:/AeroQt/transpbg/entry/hover.png) 4 4 4 4 repeat; }"
        "QLineEdit#controlPanelSearch:focus { border-image: none; border: 1px solid #7EB4EA;"
        " border-radius: 2px; background: #FFFFFF; }"
        "QLineEdit#controlPanelSearch QToolButton { border: none; border-image: none; background: transparent; }");
    m_searchBox->setClearButtonEnabled(true);
    auto *buildInfo = new QToolButton;
    buildInfo->setObjectName(QStringLiteral("controlPanelBuildInfo"));
    buildInfo->setText(QStringLiteral("?"));
    buildInfo->setAccessibleName(tr("About this Control Panel build"));
    buildInfo->setToolTip(QString::fromUtf8(release::versionText()));
    connect(buildInfo, &QToolButton::clicked, this, [this]() {
        QMessageBox box(QMessageBox::Information, tr("About Control Panel"),
            QString::fromUtf8(release::versionText()), QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.setInformativeText(tr("Build: %1\nSource revision: %2\nSource manifest SHA-256: %3")
            .arg(QString::fromUtf8(release::buildId()), QString::fromUtf8(release::sourceRevision()),
                 QString::fromUtf8(release::sourceManifestSha256())));
        box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        box.exec();
    });
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(180);
    // Assistive clients edit through Qt's accessibility text interface, which
    // emits textChanged without textEdited. Internal restoration is signal-
    // blocked by syncSearchBox; pending navigation keeps the field read-only.
    connect(m_searchBox, &QLineEdit::textChanged, this, [this]() {
        if (!m_searchBox->isReadOnly()) m_searchTimer->start();
    });
    connect(m_searchTimer, &QTimer::timeout, this, [this]() { navigateSearch(m_searchBox->text()); });
    connect(m_searchBox, &QLineEdit::returnPressed, this, [this]() {
        m_searchTimer->stop();
        navigateSearch(m_searchBox->text());
    });
    m_searchBox->addAction(themeIcon({"system-search"}), QLineEdit::TrailingPosition);

    // Italic only while the placeholder shows; upright once the user types.
    QObject::connect(m_searchBox, &QLineEdit::textChanged, m_searchBox,
        [this](const QString &text) {
            QFont f = m_searchBox->font();
            f.setItalic(text.isEmpty());
            m_searchBox->setFont(f);
        });
    QFont searchFont = m_searchBox->font();
    searchFont.setPointSize(9);
    searchFont.setItalic(true);
    m_searchBox->setFont(searchFont);

    layout->addWidget(m_searchBox);
    layout->addSpacing(4);
    layout->addWidget(buildInfo);
}

void MainWindow::setCrumbTrail(const QStringList &trail)
{
    while (QLayoutItem *item = m_pathLayout->takeAt(0)) {
        if (auto *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    auto addArrow = [this]() {
        m_pathLayout->addWidget(Win7::arrowLabel(Qt::RightArrow, QColor(0x66, 0x66, 0x66), 6));
    };
    auto *controlIcon = new QLabel;
    controlIcon->setPixmap(themeIcon({"preferences-system"}).pixmap(16, 16));
    m_pathLayout->addWidget(controlIcon);
    addArrow();
    auto *home = new LinkLabel(tr("Control Panel"));
    home->setObjectName("crumbHome");
    home->setStyleSheet("QLabel { color: #000000; background: transparent; } QLabel:hover { color: #003399; }");
    m_homeCrumb = home;
    connect(home, &LinkLabel::clicked, this, &MainWindow::navigateHome, Qt::QueuedConnection);
    m_pathLayout->addWidget(home);
    for (int i = 0; i < trail.size(); ++i) {
        addArrow();
        if (i == trail.size() - 1) {
            auto *label = new QLabel(Branding::brand(trail[i]));
            label->setTextFormat(Qt::PlainText);
            label->setStyleSheet("color: #000000;");
            m_pathLayout->addWidget(label);
        } else {
            auto *link = new LinkLabel(Branding::brand(trail[i]));
            link->setStyleSheet("QLabel { color: #000000; background: transparent; } QLabel:hover { color: #003399; }");
            const QString path = trail.mid(0, i + 1).join('/');
            connect(link, &LinkLabel::clicked, this, [this, path]() { navigateTo(path); }, Qt::QueuedConnection);
            m_pathLayout->addWidget(link);
        }
    }
    m_pathLayout->addStretch();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!QApplication::activeModalWidget() && widget && widget->window() == this && widget->isEnabled()) {
            if (mouse->button() == Qt::XButton1) { goBack(); return true; }
            if (mouse->button() == Qt::XButton2) { goForward(); return true; }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// Breadcrumb paths for the pages MainWindow routes on, derived from each page's
// canonical PageId so the strings live in exactly one place (PageRegistry). The
// select-updates entry is Linux Update's in-window "select updates" sub-view.
static const QString kUpdatePath           = PageRegistry::pathFor(PageId::LinuxUpdate);
static const QString kSelectPath           = PageRegistry::pathFor(PageId::SelectUpdates);
static const QString kPerformancePath      = PageRegistry::pathFor(PageId::Performance);

// A sub-path (one containing '/') is routable only if showEntry knows how to
// render it. Category-level paths are validated separately via detailGroupsFor.
static bool isRoutableSubPath(const QString &path)
{
    const PageId id = PageRegistry::idForPath(path);
    return id != PageId::None && id != PageId::Home;
}

void MainWindow::navigateHome()
{
    requestNavigation([this]() {
        pushHistory(QString());
        showEntry(QString());
    });
}

void MainWindow::navigateTo(const QString &path)
{
    if (path.contains('/')) {
        // Sub-path: if we can't render it (e.g. an intermediate crumb like
        // "Programs/Programs and Features" with no page of its own), fall back
        // to its parent category page rather than showing a blank pane.
        if (!isRoutableSubPath(path)) {
            const QString category = path.section('/', 0, 0);
            if (detailGroupsFor(category))
                navigateTo(category);
            return;
        }
    } else if (!detailGroupsFor(path)) {
        // Category-level path with no detail page: nothing to show.
        return;
    }
    requestNavigation([this, path]() {
        playNavigationSound();
        pushHistory(path);
        showEntry(path);
    });
}

void MainWindow::syncSearchBox()
{
    const QSignalBlocker blocker(m_searchBox);
    m_searchBox->setText(m_visibleSearch);
    QFont font = m_searchBox->font();
    font.setItalic(m_visibleSearch.isEmpty());
    m_searchBox->setFont(font);
}

void MainWindow::focusDestination()
{
    auto *page = m_scroll->widget();
    if (!page) return;
    if (auto *view = page->findChild<CatalogView *>()) {
        view->setFocus(Qt::TabFocusReason);
        return;
    }
    QWidget *candidate = page->nextInFocusChain();
    while (candidate != page) {
        if (page->isAncestorOf(candidate) && candidate->isVisible() && candidate->isEnabled()
            && (candidate->focusPolicy() & Qt::TabFocus)) {
            candidate->setFocus(Qt::TabFocusReason);
            return;
        }
        candidate = candidate->nextInFocusChain();
    }
    m_homeCrumb->setFocus(Qt::TabFocusReason);
}

void MainWindow::navigateSearch(const QString &text)
{
    const QString query = text.trimmed();
    requestNavigation([this, query]() {
        const QString entry = query.isEmpty() ? QString()
            : searchPrefix + QString::fromLatin1(QUrl::toPercentEncoding(query));
        // Editing a query is one history step, so Back returns to the page
        // where the search began instead of walking through each keystroke.
        if (!query.isEmpty() && m_historyIndex >= 0
            && m_history.at(m_historyIndex).startsWith(searchPrefix)) {
            if (m_history.at(m_historyIndex) != entry) {
                while (m_history.size() > m_historyIndex + 1) m_history.removeLast();
                m_history[m_historyIndex] = entry;
            }
        } else {
            pushHistory(entry);
        }
        showEntry(entry);
        updateNavButtons();
    });
}

QWidget *MainWindow::buildSearchPage(const QString &query)
{
    auto *page = new QWidget;
    page->setObjectName("searchPage");
    page->setStyleSheet("#searchPage { background: #FFFFFF; }");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 16, 24, 16);
    auto *heading = Win7::pageTitle(tr("Search Control Panel"));
    layout->addWidget(heading);
    auto *view = new CatalogView(CatalogView::Mode::Search, query);
    connect(view, &CatalogView::actionActivated, this, &MainWindow::activateAction, Qt::QueuedConnection);
    layout->addWidget(view, 1);
    return page;
}

void MainWindow::setViewMode(ViewMode mode)
{
    requestNavigation([this, mode]() {
        m_viewMode = mode;
        QSettings preferences;
        preferences.setValue(QStringLiteral("shell/viewMode"), static_cast<int>(mode));
        pushHistory(QString());
        showEntry(QString());
        if (auto *button = m_scroll->widget()->findChild<QToolButton *>(QStringLiteral("viewBy")))
            button->setFocus();
    });
}

void MainWindow::openApplet(const QString &id)
{
    // Applet ids are "base[:view]", e.g. "datetime:additional".
    const QString base = id.section(':', 0, 0);
    const QString view = id.section(':', 1, 1);
    playNavigationSound();

    if (base == QLatin1String("datetime")) {
        DateTimeDialog dlg(this);
        if (view == QLatin1String("additional"))
            dlg.showTab(DateTimeDialog::TabAdditionalClocks);
        else if (view == QLatin1String("internet"))
            dlg.showTab(DateTimeDialog::TabInternetTime);
        dlg.exec();
    } else if (base == QLatin1String("sound")) {
        auto *settings = AudioSettings::instance();
        registerAudioSettings(settings);
        SoundDialog dlg(settings, this);
        if (view == QLatin1String("recording"))
            dlg.showTab(SoundDialog::TabRecording);
        else if (view == QLatin1String("sounds"))
            dlg.showTab(SoundDialog::TabSounds);
        else if (view == QLatin1String("communications"))
            dlg.showTab(SoundDialog::TabCommunications);
        else if (view == QLatin1String("applications"))
            dlg.showTab(SoundDialog::TabApplications);
        dlg.exec();
    }
}

// Render an entry without touching history. Empty string == home page.
void MainWindow::showEntry(const QString &entry)
{
    QWidget *focus = QApplication::focusWidget();
    const bool moveFocus = focus && ((m_scroll->widget()
        && (focus == m_scroll->widget() || m_scroll->widget()->isAncestorOf(focus)))
        || (m_homeCrumb && m_homeCrumb->parentWidget()->isAncestorOf(focus)));
    // Retain typing focus while rebuilding results; move content focus to the
    // destination after its old owner has been removed from the scroll area.
    if (moveFocus) QTimer::singleShot(0, this, &MainWindow::focusDestination);
    m_searchTimer->stop();
    m_visibleSearch = searchQuery(entry);
    syncSearchBox();
    // Moving between the status view and the select-updates view reuses the
    // live page so the checked-update state survives; only the crumb trail and
    // the page's internal view switch. Recreating it would reset the checks.
    if ((entry == kUpdatePath || entry == kSelectPath)
        && m_updatePage && m_scroll->widget() == m_updatePage) {
        setCrumbTrail(entry.split('/'));
        if (entry == kSelectPath) m_updatePage->showSelectView();
        else                      m_updatePage->showStatusView();
        return;
    }

    // Page views can be replaced while their services retain state and jobs.
    m_sidebarTextEffect  = nullptr;
    m_updatePage         = nullptr;

    if (entry.startsWith(searchPrefix)) {
        setCrumbTrail({tr("Search results")});
        m_scroll->setWidget(buildSearchPage(m_visibleSearch));
    } else if (entry.isEmpty()) {
        setCrumbTrail(m_viewMode == ViewMode::Category ? QStringList{} : QStringList{tr("All Control Panel Items")});
        m_scroll->setWidget(buildHomePage());
    } else if (entry.contains('/')) {
        // Sub-page path: "Category/SubPage[/SubView]"
        setCrumbTrail(entry.split('/'));
        const PageId page = PageRegistry::idForPath(entry);
        if (page == PageId::LinuxUpdate || page == PageId::SelectUpdates) {
            auto *sidebar = buildSubpageSidebar(
                LinuxUpdatePage::sidebarLinks(),
                LinuxUpdatePage::sidebarSeeAlso());
            auto *wuPage = new LinuxUpdatePage(sidebar);
            m_updatePage = wuPage;
            QObject::connect(wuPage, &LinuxUpdatePage::navigateHomeRequested,
                             this, &MainWindow::navigateHome, Qt::QueuedConnection);
            QObject::connect(wuPage, &LinuxUpdatePage::selectUpdatesRequested,
                             this, [this]() { navigateTo(kSelectPath); },
                             Qt::QueuedConnection);
            QObject::connect(wuPage, &LinuxUpdatePage::selectViewClosed,
                             this, &MainWindow::goBack, Qt::QueuedConnection);
            QObject::connect(wuPage, &LinuxUpdatePage::controlPanelInstallRequested,
                             this, &MainWindow::requestControlPanelUpdateInstall, Qt::QueuedConnection);
            QObject::connect(wuPage, &QObject::destroyed, this,
                             [this]() { m_updatePage = nullptr; });
            m_scroll->setWidget(wuPage);
            if (entry == kSelectPath) m_updatePage->showSelectView();
        } else if (page == PageId::System) {
            auto *sidebar = buildSubpageSidebar(
                SystemPage::sidebarLinks(),
                SystemPage::sidebarSeeAlso());
            auto *sysPage = new SystemPage(sidebar);
            QObject::connect(sysPage, &SystemPage::performanceRequested, this,
                             [this]() { navigateTo(kPerformancePath); },
                             Qt::QueuedConnection);
            // Re-navigate after the branding dialog so the new wording/badge
            // (and every other page reached from here) takes effect live.
            QObject::connect(sysPage, &SystemPage::brandingChanged, this,
                             [this, entry]() { navigateTo(entry); },
                             Qt::QueuedConnection);
            m_scroll->setWidget(sysPage);
        } else if (page == PageId::Performance) {
            auto *sidebar = buildSubpageSidebar(
                PerformancePage::sidebarLinks(),
                PerformancePage::sidebarSeeAlso());
            m_scroll->setWidget(new PerformancePage(sidebar));
        } else if (page == PageId::InstalledUpdates) {
            auto *sidebar = buildSubpageSidebar(
                InstalledUpdatesPage::sidebarLinks(),
                InstalledUpdatesPage::sidebarSeeAlso());
            m_scroll->setWidget(new InstalledUpdatesPage(sidebar));
        } else if (page == PageId::ProgramsFeatures) {
            auto *sidebar = buildSubpageSidebar(
                ProgramsFeaturesPage::sidebarLinks(),
                ProgramsFeaturesPage::sidebarSeeAlso());
            m_scroll->setWidget(new ProgramsFeaturesPage(sidebar));
        } else if (page == PageId::NetworkSharing) {
            auto *sidebar = buildSubpageSidebar(
                NetworkSharingPage::sidebarLinks(),
                NetworkSharingPage::sidebarSeeAlso());
            auto *network = new NetworkSharingPage(sidebar);
            connect(network, &NetworkSharingPage::navigationRequested, this, [this](PageId id) { navigateTo(PageRegistry::pathFor(id)); });
            m_scroll->setWidget(network);
        } else if (page == PageId::NetworkConnections) {
            auto *sidebar = buildSubpageSidebar(NetworkConnectionsPage::sidebarLinks(), NetworkConnectionsPage::sidebarSeeAlso());
            auto *network = new NetworkConnectionsPage(sidebar);
            connect(network, &NetworkConnectionsPage::navigationRequested, this, [this](PageId id) { navigateTo(PageRegistry::pathFor(id)); });
            m_scroll->setWidget(network);
        } else if (page == PageId::CustomShortcuts) {
            auto *sidebar = buildSubpageSidebar(CustomShortcutsPage::sidebarLinks(), CustomShortcutsPage::sidebarSeeAlso());
            auto *session = m_importCoordinator ? m_importCoordinator->customSession() : CustomShortcutsSettings::instance();
            if (session) {
                auto *custom = new CustomShortcutsPage(sidebar, session);
                connect(custom, &CustomShortcutsPage::navigationRequested, this, [this](PageId id) { navigateTo(PageRegistry::pathFor(id)); });
                m_scroll->setWidget(custom);
            } else m_scroll->setWidget(new QLabel(tr("The custom shortcut session is unavailable. The import review is retained.")));
        } else if (page == PageId::StandardShortcuts || page == PageId::Shortcuts) {
            const auto scope = page == PageId::StandardShortcuts ? ShortcutsPage::Scope::StandardApplications : ShortcutsPage::Scope::Global;
            auto *sidebar = buildSubpageSidebar(ShortcutsPage::sidebarLinks(scope), ShortcutsPage::sidebarSeeAlso());
            if (!m_importCoordinator) setShortcutImportCoordinator(ShortcutImportCoordinator::instance());
            auto *globals = m_importCoordinator->globalSession(), *standards = m_importCoordinator->standardSession();
            auto *session = scope == ShortcutsPage::Scope::Global ? globals : standards;
            if (session) {
                auto *shortcuts = new ShortcutsPage(sidebar, session, scope);
                shortcuts->setCompanionSession(scope == ShortcutsPage::Scope::Global ? standards : globals);
                shortcuts->setCustomSession(m_importCoordinator->customSession());
                shortcuts->setImportCoordinator(m_importCoordinator);
                connect(shortcuts, &ShortcutsPage::navigationRequested, this, [this](PageId id) { navigateTo(PageRegistry::pathFor(id)); });
                m_scroll->setWidget(shortcuts);
            } else m_scroll->setWidget(new QLabel(tr("The shortcut session is unavailable. The import review is retained.")));
        } else if (page == PageId::Keyboard) {
            auto *sidebar = buildSubpageSidebar(KeyboardPage::sidebarLinks(), KeyboardPage::sidebarSeeAlso());
            m_scroll->setWidget(new KeyboardPage(sidebar));
        } else if (page == PageId::DefaultApps) {
            auto *sidebar = buildSubpageSidebar(DefaultAppsPage::sidebarLinks(), DefaultAppsPage::sidebarSeeAlso());
            m_scroll->setWidget(new DefaultAppsPage(sidebar));
        } else if (page == PageId::InputDevices) {
            auto *sidebar = buildSubpageSidebar(InputDevicesPage::sidebarLinks(), InputDevicesPage::sidebarSeeAlso());
            m_scroll->setWidget(new InputDevicesPage(sidebar));
        } else if (page == PageId::Firewall) {
            auto *sidebar = buildSubpageSidebar(
                FirewallPage::sidebarLinks(),
                FirewallPage::sidebarSeeAlso());
            m_scroll->setWidget(new FirewallPage(sidebar));
        } else if (page == PageId::ActionCenter) {
            auto *sidebar = buildSubpageSidebar(
                ActionCenterPage::sidebarLinks(),
                ActionCenterPage::sidebarSeeAlso());
            m_scroll->setWidget(new ActionCenterPage(sidebar));
        } else if (page == PageId::PowerOptions) {
            auto *sidebar = buildSubpageSidebar(
                PowerOptionsPage::sidebarLinks(),
                PowerOptionsPage::sidebarSeeAlso());
            m_scroll->setWidget(new PowerOptionsPage(sidebar));
        } else if (page == PageId::AeroLogin) {
            auto *sidebar = buildSubpageSidebar(AeroLoginPage::sidebarLinks(), AeroLoginPage::sidebarSeeAlso());
            auto *appearance = new AeroLoginPage(sidebar);
            connect(appearance, &AeroLoginPage::navigationRequested, this, [this](PageId id) { navigateTo(PageRegistry::pathFor(id)); }, Qt::QueuedConnection);
            m_scroll->setWidget(appearance);
        } else if (page == PageId::ScreenLock) {
            auto *sidebar = buildSubpageSidebar(ScreenLockPage::sidebarLinks(), ScreenLockPage::sidebarSeeAlso());
            auto *screenLock = new ScreenLockPage(sidebar);
            connect(screenLock, &ScreenLockPage::appearanceRequested, this, [this] { navigateTo(PageRegistry::pathFor(PageId::AeroLogin)); }, Qt::QueuedConnection);
            connect(screenLock, &ScreenLockPage::editLockShortcutRequested, this, [this] {
                requestNavigation([this] {
                    const auto path = PageRegistry::pathFor(PageId::Shortcuts);
                    playNavigationSound(); pushHistory(path); showEntry(path);
                    if (auto *shortcuts = qobject_cast<ShortcutsPage *>(m_scroll->widget()))
                        shortcuts->focusExistingAction(QStringLiteral("ksmserver"), QStringLiteral("Lock Session"));
                });
            }, Qt::QueuedConnection);
            m_scroll->setWidget(screenLock);
        } else if (page == PageId::NightTime) {
            auto *sidebar = buildSubpageSidebar(NightTimePage::sidebarLinks(), NightTimePage::sidebarSeeAlso());
            m_scroll->setWidget(new NightTimePage(sidebar));
        } else if (page == PageId::NightLight) {
            auto *sidebar = buildSubpageSidebar(NightLightPage::sidebarLinks(), NightLightPage::sidebarSeeAlso());
            auto *nightLight = new NightLightPage(sidebar);
            connect(nightLight, &NightLightPage::navigationRequested, this, [this](PageId id) { navigateTo(PageRegistry::pathFor(id)); });
            m_scroll->setWidget(nightLight);
        } else if (page == PageId::Display) {
            auto *sidebar = buildSubpageSidebar(DisplayPage::sidebarLinks(), DisplayPage::sidebarSeeAlso());
            m_scroll->setWidget(new DisplayPage(sidebar));
        } else if (page == PageId::WindowBehavior) {
            auto *sidebar = buildSubpageSidebar(WindowBehaviorPage::sidebarLinks(), WindowBehaviorPage::sidebarSeeAlso());
            m_scroll->setWidget(new WindowBehaviorPage(sidebar));
        } else if (page == PageId::DesktopBackground) {
            auto *sidebar = buildSubpageSidebar(DesktopBackgroundPage::sidebarLinks(), DesktopBackgroundPage::sidebarSeeAlso());
            m_scroll->setWidget(new DesktopBackgroundPage(sidebar));
        } else if (page == PageId::ThemeComponents) {
            auto *sidebar = buildSubpageSidebar(ThemeComponentsPage::sidebarLinks(), ThemeComponentsPage::sidebarSeeAlso());
            auto *components = new ThemeComponentsPage(sidebar);
            connect(components, &ThemeComponentsPage::navigationRequested, this, [this](PageId id) { navigateTo(PageRegistry::pathFor(id)); });
            m_scroll->setWidget(components);
        } else if (page == PageId::FontSettings) {
            auto *sidebar = buildSubpageSidebar(FontSettingsPage::sidebarLinks(), FontSettingsPage::sidebarSeeAlso());
            m_scroll->setWidget(new FontSettingsPage(sidebar));
        } else if (page == PageId::AeroEffects) {
            auto *sidebar = buildSubpageSidebar(PersonalizationPage::sidebarLinks(), PersonalizationPage::sidebarSeeAlso());
            auto *personalization = new PersonalizationPage(sidebar);
            connect(personalization, &PersonalizationPage::navigationRequested, this, [this](PageId id) { navigateTo(PageRegistry::pathFor(id)); });
            m_scroll->setWidget(personalization);
        } else if (page == PageId::PackagePreview) {
            auto *sidebar = buildSubpageSidebar(PackagePreviewPage::sidebarLinks(), PackagePreviewPage::sidebarSeeAlso());
            auto *preview = new PackagePreviewPage(sidebar);
            m_scroll->setWidget(preview);
            registerPackageSimulation(preview->simulationService());
        } else if (page == PageId::Personalization) {
            auto *sidebar = buildSubpageSidebar(
                PersonalizationPage::sidebarLinks(),
                PersonalizationPage::sidebarSeeAlso());
            auto *personalization = new PersonalizationPage(sidebar);
            connect(personalization, &PersonalizationPage::navigationRequested, this, [this](PageId id) { navigateTo(PageRegistry::pathFor(id)); });
            m_scroll->setWidget(personalization);
        } else if (page == PageId::Fonts) {
            auto *sidebar = buildSubpageSidebar(
                FontsPage::sidebarLinks(),
                FontsPage::sidebarSeeAlso());
            m_scroll->setWidget(new FontsPage(sidebar));
            registerFontService(UserFontService::instance());
        } else if (page == PageId::UserAccounts) {
            auto *sidebar = buildSubpageSidebar(
                UserAccountsPage::sidebarLinks(),
                UserAccountsPage::sidebarSeeAlso());
            m_scroll->setWidget(new UserAccountsPage(sidebar));
        } else if (page == PageId::EaseOfAccess) {
            auto *sidebar = buildSubpageSidebar(
                EaseOfAccessPage::sidebarLinks(),
                EaseOfAccessPage::sidebarSeeAlso());
            auto *access = new EaseOfAccessPage(sidebar);
            connect(access, &EaseOfAccessPage::navigationRequested, this, [this](PageId id) { navigateTo(PageRegistry::pathFor(id)); });
            m_scroll->setWidget(access);
        } else if (page == PageId::DevicesPrinters) {
            // A shell folder, not a Control Panel item: no left-nav sidebar.
            m_scroll->setWidget(new DevicesAndPrintersPage());
        }
    } else {
        setCrumbTrail({ entry });
        m_scroll->setWidget(buildCategoryPage(entry));
    }
    if (auto *session = currentSettingsSession())
        registerSettingsSession(session, Branding::brand(entry.section('/', -1)));
}

void MainWindow::pushHistory(const QString &entry)
{
    // Re-navigating to the current location shouldn't grow history.
    if (m_historyIndex >= 0 && m_history.at(m_historyIndex) == entry)
        return;
    // Drop any forward entries before branching to a new location.
    while (m_history.size() > m_historyIndex + 1)
        m_history.removeLast();
    m_history.append(entry);
    m_historyIndex = m_history.size() - 1;
    updateNavButtons();
}

void MainWindow::goBack()
{
    if (m_historyIndex <= 0)
        return;
    requestNavigation([this]() {
        --m_historyIndex;
        showEntry(m_history.at(m_historyIndex));
        updateNavButtons();
    });
}

void MainWindow::goForward()
{
    if (m_historyIndex >= m_history.size() - 1)
        return;
    requestNavigation([this]() {
        ++m_historyIndex;
        showEntry(m_history.at(m_historyIndex));
        updateNavButtons();
    });
}

void MainWindow::updateNavButtons()
{
    if (m_backBtn)
        m_backBtn->setEnabled(m_historyIndex > 0 && !m_navigationGuard->pending());
    if (m_forwardBtn)
        m_forwardBtn->setEnabled(m_historyIndex < m_history.size() - 1 && !m_navigationGuard->pending());
}

SettingsSession *MainWindow::currentSettingsSession() const
{
    if (auto *page = qobject_cast<CustomShortcutsPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<NetworkSharingPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<NetworkConnectionsPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<ShortcutsPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<KeyboardPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<DefaultAppsPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<InputDevicesPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<PersonalizationPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<ScreenLockPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<AeroLoginPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<NightTimePage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<NightLightPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<DisplayPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<PowerOptionsPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<WindowBehaviorPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<DesktopBackgroundPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<ThemeComponentsPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<FontSettingsPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<UserAccountsPage *>(m_scroll->widget()))
        return page->settingsSession();
    if (auto *page = qobject_cast<AeroEffectsPage *>(m_scroll->widget()))
        return page->settingsSession();
    return nullptr;
}

ShortcutImportCoordinator *MainWindow::shortcutImportCoordinator() const { return m_importCoordinator; }
void MainWindow::setShortcutImportCoordinator(ShortcutImportCoordinator *coordinator)
{
    if (m_importCoordinator == coordinator) return;
    if (m_importCoordinator) disconnect(m_importCoordinator, nullptr, this, nullptr);
    m_importCoordinator = coordinator;
    if (coordinator) {
        connect(coordinator, &ShortcutImportCoordinator::changed, this, &MainWindow::refreshJobStrip);
        connect(coordinator, &QObject::destroyed, this, &MainWindow::refreshJobStrip);
        registerSettingsSession(coordinator->globalSession(), tr("Keyboard Shortcuts"));
        registerSettingsSession(coordinator->standardSession(), tr("Application Shortcuts"));
        registerSettingsSession(coordinator->customSession(), tr("Custom Keyboard Shortcuts"));
    }
    refreshJobStrip();
}
void MainWindow::openShortcutImport()
{
    if (!m_importCoordinator || QApplication::activeModalWidget()) return;
    auto *dialog = new ShortcutSchemeDialog(m_importCoordinator->globalSession(), m_importCoordinator->standardSession(),
        ShortcutSchemeDialog::Mode::Import, this);
    dialog->setCustomSession(m_importCoordinator->customSession());
    dialog->setImportCoordinator(m_importCoordinator);
    dialog->setBuiltInSchemes(ShortcutScheme::builtInSchemes());
    connect(dialog, &ShortcutSchemeDialog::navigationRequested, this, [this](PageId id) {
        QTimer::singleShot(0, this, [this, id] { navigateTo(PageRegistry::pathFor(id)); });
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->open();
}

void MainWindow::registerSettingsSession(SettingsSession *session, const QString &title)
{
    if (!session) return;
    for (const auto &entry : m_settingsSessions)
        if (entry.session == session) return;
    m_settingsSessions.append({session, title});
    connect(session, &SettingsSession::stateChanged, this, &MainWindow::refreshJobStrip);
    connect(session, &SettingsSession::applyFinished, this, [this, session, title](bool success) {
        QString outcome = session->errorText();
        if (outcome.isEmpty()) outcome = session->resultText();
        if (outcome.isEmpty()) outcome = success ? tr("Completed") : tr("No change was kept");
        m_settingsResult = QStringLiteral("%1: %2").arg(title, outcome);
        refreshJobStrip();
    });
    refreshJobStrip();
}

void MainWindow::registerFontService(UserFontService *service)
{
    if (!service || m_fontService == service) return;
    if (m_fontService) disconnect(m_fontService, nullptr, this, nullptr);
    m_fontService = service;
    connect(service, &UserFontService::changed, this, [this] {
        if (m_fontService && !m_fontService->busy()) m_settingsResult = tr("Fonts: %1").arg(m_fontService->result());
        refreshJobStrip();
    });
    refreshJobStrip();
}

void MainWindow::registerPackageSimulation(PackageSimulationService *service)
{
    if (!service || m_packageSimulation == service) return;
    if (m_packageSimulation) disconnect(m_packageSimulation, nullptr, this, nullptr);
    m_packageSimulation = service;
    connect(service, &PackageSimulationService::changed, this, [this] {
        if (m_packageSimulation && !m_packageSimulation->isBusy())
            m_settingsResult = tr("Ubuntu Package Preview: %1").arg(m_packageSimulation->statusText());
        refreshJobStrip();
    });
    refreshJobStrip();
}

void MainWindow::registerAudioSettings(AudioSettings *settings)
{
    if (!settings || m_audioSettings == settings) return;
    if (m_audioSettings) disconnect(m_audioSettings, nullptr, this, nullptr);
    m_audioSettings = settings;
    connect(settings, &AudioSettings::stateChanged, this, [this] {
        if (m_audioSettings && !m_audioSettings->isBusy()) {
            const auto outcome = !m_audioSettings->errorText().isEmpty() ? m_audioSettings->errorText() : m_audioSettings->resultText();
            if (!outcome.isEmpty()) m_settingsResult = tr("Sound: %1").arg(outcome);
        }
        refreshJobStrip();
    });
    refreshJobStrip();
}

void MainWindow::requestNavigation(std::function<void()> action)
{
    if (QApplication::activeModalWidget()) return;
    m_navigationGuard->request(currentSettingsSession(), [this]() {
        QMessageBox box(QMessageBox::Question, QStringLiteral("Unsaved settings"),
            QStringLiteral("Apply your changes before leaving this page?"),
            QMessageBox::NoButton, this);
        auto *apply = box.addButton(QStringLiteral("Apply"), QMessageBox::AcceptRole);
        auto *discard = box.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
        auto *cancel = box.addButton(QMessageBox::Cancel);
        const auto reviewed=pinAeroLoginReview(box,apply,currentSettingsSession());
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() == apply && reviewed()) return SettingsNavigationGuard::Choice::Apply;
        if (box.clickedButton() == discard) return SettingsNavigationGuard::Choice::Discard;
        return SettingsNavigationGuard::Choice::Cancel;
    }, std::move(action));
}

void MainWindow::playNavigationSound()
{
    if (!m_soundsEnabled) return;
    const QString path = QStringLiteral("/usr/share/sounds/Windows 7/og/Windows Navigation Start.wav");
    if (!QFileInfo::exists(path)) return;
    if (!m_navSound) {
        m_navSound = new QSoundEffect(this);
        m_navSound->setSource(QUrl::fromLocalFile(path));
        m_navSound->setVolume(1.0f);
    }
    m_navSound->play();
}

bool MainWindow::hasBackgroundWork() const
{
    for (const auto &entry : m_settingsSessions)
        if (settingsWork(entry.session)) return true;
    if (settingsWork(currentSettingsSession())) return true;
    return OperationManager::instance()->hasActiveJobs()
        || (m_audioSettings && m_audioSettings->isBusy())
        || (m_packageSimulation && m_packageSimulation->isBusy())
        || ProgramsService::instance()->isLoading() || ProgramsService::instance()->isActing()
        || PowerProfileService::instance()->isBusy() || WeiBenchmark::instance()->isRunning();
}

void MainWindow::requestControlPanelUpdateInstall()
{
    // Only this explicit action starts the detached installer. Ordinary Close,
    // navigation, cancelled dialogs and a previous download never schedule it.
    if (hasBackgroundWork() || m_navigationGuard->pending()) {
        QMessageBox::information(this, tr("Control Panel update"),
            tr("Wait for background work to finish before installing the Control Panel update."));
        return;
    }
    bool dirty = currentSettingsSession() && currentSettingsSession()->isDirty();
    for (const auto &entry : m_settingsSessions) dirty |= entry.session && entry.session->isDirty();
    dirty |= m_importCoordinator && m_importCoordinator->hasPendingWork();
    if (dirty) {
        QMessageBox::information(this, tr("Control Panel update"),
            tr("Apply or discard your pending changes before choosing Close and install. "
               "Control Panel will stay open."));
        return;
    }
    QString error;
    if (!ControlPanelUpdateService::instance()->launchInstaller(&error)) {
        QMessageBox::information(this, tr("Control Panel update"), error);
        return;
    }
    // The installer still waits for the process-wide shared lock to be released.
    // Existing closeEvent guards remain authoritative; no force-close is used.
    close();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (hasBackgroundWork()) {
        event->ignore();
        QMessageBox::information(this, QStringLiteral("Background work is running"),
            QStringLiteral("Wait for background work to finish before closing Control Panel. "
                           "You can continue using other pages while it runs."));
        return;
    }
    bool dirty = currentSettingsSession() && currentSettingsSession()->isDirty();
    for (const auto &entry : m_settingsSessions) dirty |= entry.session && entry.session->isDirty();
    dirty |= m_importCoordinator && m_importCoordinator->hasPendingWork();
    if (!m_navigationGuard->pending() && !dirty) {
        event->accept();
        return;
    }
    event->ignore();
    requestCloseApproval();
}

void MainWindow::requestCloseApproval()
{
    if (QApplication::activeModalWidget() || m_navigationGuard->pending()) return;
    SettingsSession *selected = currentSettingsSession(); QString title = tr("Settings");
    if (!selected || !selected->isDirty()) selected = nullptr;
    for (const auto &entry : m_settingsSessions) {
        if (!entry.session || !entry.session->isDirty()) continue;
        if (!selected) { selected = entry.session; title = entry.title; break; }
        if (entry.session == selected) { title = entry.title; break; }
    }
    if (!selected) {
        if (m_importCoordinator && m_importCoordinator->hasPendingWork()) {
            QMessageBox box(QMessageBox::Question, tr("Unfinished shortcut import"),
                tr("Discard the retained import review and close Control Panel? Settings already applied remain saved."),
                QMessageBox::NoButton, this);
            auto *discard = box.addButton(tr("Discard import review"), QMessageBox::DestructiveRole);
            auto *cancel = box.addButton(QMessageBox::Cancel); box.setDefaultButton(cancel); box.exec();
            if (box.clickedButton() != discard) return;
            if (m_importCoordinator) m_importCoordinator->discard();
        }
        QTimer::singleShot(0, this, [this] { close(); }); return;
    }
    m_navigationGuard->request(selected, [this, title, selected=QPointer<SettingsSession>(selected)] {
        QMessageBox box(QMessageBox::Question, tr("Unsaved settings"),
            tr("Apply changes to %1 before closing Control Panel?").arg(title), QMessageBox::NoButton, this);
        auto *apply = box.addButton(tr("Apply"), QMessageBox::AcceptRole);
        auto *discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        auto *cancel = box.addButton(QMessageBox::Cancel);
        const auto reviewed=pinAeroLoginReview(box,apply,selected);
        box.setDefaultButton(cancel); box.exec();
        if (box.clickedButton() == apply && reviewed()) return SettingsNavigationGuard::Choice::Apply;
        if (box.clickedButton() == discard) return SettingsNavigationGuard::Choice::Discard;
        return SettingsNavigationGuard::Choice::Cancel;
    }, [this] { QTimer::singleShot(0, this, &MainWindow::requestCloseApproval); });
}

void MainWindow::refreshJobStrip()
{
    auto *manager = OperationManager::instance();
    QStringList titles = manager->activityTitles();
    for (auto *job : manager->jobs()) if (job->active()) titles << job->title();
    if (m_packageSimulation && m_packageSimulation->isBusy()) titles << tr("Previewing Ubuntu package changes");
    if (ProgramsService::instance()->isLoading()) titles << QStringLiteral("Loading installed programs");
    if (ProgramsService::instance()->isActing() && titles.isEmpty()) titles << QStringLiteral("Package transaction");
    const bool retainedImport = m_importCoordinator && m_importCoordinator->hasDocument();
    m_resumeImport->setVisible(retainedImport);
    bool settingsDetails = false; QStringList pendingTitles;
    for (const auto &entry : m_settingsSessions) {
        if (!entry.session) continue;
        if (auto *network = qobject_cast<NetworkSettings *>(entry.session.data()); network && network->isScanning())
            titles << tr("Scanning wireless networks");
        else if (entry.session->isBusy()) titles << entry.title;
        if (entry.session->isDirty() && !entry.session->isBusy()) pendingTitles << entry.title;
        settingsDetails |= entry.session->isDirty() || !entry.session->errorText().isEmpty() || !entry.session->resultText().isEmpty();
    }
    if (PowerProfileService::instance()->isBusy()) titles << QStringLiteral("Changing power profile");
    if (WeiBenchmark::instance()->isRunning()) titles << WeiBenchmark::instance()->phaseText();
    if (!titles.isEmpty()) {
        m_jobLabel->setText(QStringLiteral("Working: %1").arg(titles.join(QStringLiteral("; "))));
    } else if (!pendingTitles.isEmpty()) {
        m_jobLabel->setText(tr("Pending changes: %1").arg(pendingTitles.join(QStringLiteral("; "))));
    } else if (!manager->jobs().isEmpty()) {
        auto *last = manager->jobs().last();
        m_jobLabel->setText(QStringLiteral("%1: %2").arg(last->title(),
            last->succeeded() ? QStringLiteral("completed") : QStringLiteral("failed — see Details")));
    } else if (retainedImport) {
        m_jobLabel->setText(m_importCoordinator->statusText());
    }
    if (titles.isEmpty() && pendingTitles.isEmpty() && manager->jobs().isEmpty() && !retainedImport && !m_settingsResult.isEmpty())
        m_jobLabel->setText(m_settingsResult);
    m_jobDetails->setEnabled(!manager->jobs().isEmpty() || settingsDetails || retainedImport
                            || (m_audioSettings && (!m_audioSettings->errorText().isEmpty() || !m_audioSettings->resultText().isEmpty()))
                            || (m_fontService && !m_fontService->result().isEmpty())
                            || (m_packageSimulation && m_packageSimulation->state() != PackageSimulationService::State::Idle));
    m_jobStrip->setVisible(!titles.isEmpty() || !pendingTitles.isEmpty() || !manager->jobs().isEmpty() || !m_settingsResult.isEmpty() || retainedImport);
    emit backgroundStatusChanged();
}

QWidget *MainWindow::buildHomePage()
{
    // Outer: full-width white, centers the inner column
    auto *content = new QWidget;
    content->setObjectName("homeContent");
    content->setStyleSheet("#homeContent { background: #FFFFFF; }");
    auto *outerH = new QHBoxLayout(content);
    outerH->setContentsMargins(0, 0, 0, 0);
    outerH->setSpacing(0);

    // Inner column: sized to its content (the two-column grid), holds heading
    // row + grid. Centred in the white area like the real Control Panel.
    auto *inner = new QWidget;
    inner->setStyleSheet("background: transparent;");
    inner->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    if (m_viewMode == ViewMode::Category) {
        outerH->addStretch(1);
        outerH->addWidget(inner, 0, Qt::AlignTop);
        outerH->addStretch(1);
    } else {
        outerH->addWidget(inner, 1);
    }

    auto *innerV = new QVBoxLayout(inner);
    innerV->setContentsMargins(m_viewMode == ViewMode::Category ? 0 : 24, 15, m_viewMode == ViewMode::Category ? 0 : 24, 10);
    innerV->setSpacing(0);

    // Heading row: "Adjust your computer's settings"  |  View by: Category
    auto *headRow = new QHBoxLayout;
    headRow->setContentsMargins(0, 0, 0, 22);

    auto *settingsLabel = new QLabel(m_viewMode == ViewMode::Category
        ? tr("Adjust your computer's settings") : tr("All Control Panel Items"));
    QFont f = settingsLabel->font();
    f.setPointSize(12);
    settingsLabel->setFont(f);
    settingsLabel->setStyleSheet("color: #003399;");
    headRow->addWidget(settingsLabel);
    headRow->addStretch();

    auto *viewByLabel = new QLabel("View by:");
    viewByLabel->setStyleSheet("color: #333333; font-size: 9pt;");
    headRow->addWidget(viewByLabel);

    // The Win7 "View by:" dropdown: blue text ending in the shared Aero arrow,
    // orange on hover, opening its menu on click.
    const QStringList modes = {tr("Category"), tr("Large icons"), tr("Small icons")};
    auto *categoryBtn = new Win7::MenuButton(modes.at(static_cast<int>(m_viewMode)));
    categoryBtn->setObjectName("viewBy");
    categoryBtn->setAccessibleName(tr("View by: %1").arg(categoryBtn->text()));
    auto *viewMenu = new QMenu(categoryBtn);
    for (int i = 0; i < modes.size(); ++i) {
        auto *action = viewMenu->addAction(modes.at(i));
        action->setCheckable(true);
        action->setChecked(i == static_cast<int>(m_viewMode));
        connect(action, &QAction::triggered, this, [this, i]() { setViewMode(static_cast<ViewMode>(i)); }, Qt::QueuedConnection);
    }
    categoryBtn->setMenu(viewMenu);
    headRow->addWidget(categoryBtn);

    innerV->addLayout(headRow);
    if (m_viewMode != ViewMode::Category) {
        auto *view = new CatalogView(m_viewMode == ViewMode::LargeIcons
            ? CatalogView::Mode::LargeIcons : CatalogView::Mode::SmallIcons);
        connect(view, &CatalogView::actionActivated, this, &MainWindow::activateAction, Qt::QueuedConnection);
        innerV->addWidget(view, 1);
        return content;
    }

    // Two independent columns. The real Control Panel stacks each column as its
    // own list with even gaps; it does NOT align rows across the two columns, so
    // a short item never gets padded to match a tall item beside it.
    auto *columnsWidget = new QWidget;
    columnsWidget->setStyleSheet("background: transparent;");
    auto *columns = new QHBoxLayout(columnsWidget);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(28);

    auto *colLeft = new QVBoxLayout;
    colLeft->setContentsMargins(0, 0, 0, 0);
    colLeft->setSpacing(0);
    auto *colRight = new QVBoxLayout;
    colRight->setContentsMargins(0, 0, 0, 0);
    colRight->setSpacing(0);

    const QList<CategoryItem> &categories = homeCategories();
    for (int i = 0; i < categories.size(); ++i) {
        auto *w = new CategoryWidget(categories[i]);
        connect(w, &CategoryWidget::titleActivated, this, &MainWindow::navigateTo, Qt::QueuedConnection);
        connect(w, &CategoryWidget::actionActivated, this, &MainWindow::activateAction, Qt::QueuedConnection);
        (i % 2 == 0 ? colLeft : colRight)->addWidget(w);
    }
    colLeft->addStretch(1);
    colRight->addStretch(1);

    columns->addLayout(colLeft);
    columns->addLayout(colRight);

    innerV->addWidget(columnsWidget);
    innerV->addStretch(1);

    return content;
}

MainWindow::Sidebar MainWindow::buildSidebarShell(int initialWidth)
{
    auto *clip = new QScrollArea;
    clip->setFixedWidth(initialWidth);
    clip->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    clip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    clip->setFrameShape(QFrame::NoFrame);
    clip->setWidgetResizable(true);
    clip->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto *pane = new QFrame;
    pane->setObjectName("navPane");
    pane->setFixedWidth(195);
    pane->setStyleSheet(
        "#navPane { background: #F1F4F9; border-right: 1px solid #DCE0E8; }"
    );
    auto *outerV = new QVBoxLayout(pane);
    outerV->setContentsMargins(0, 0, 0, 0);
    outerV->setSpacing(0);

    // Text lives in a child widget so a fade effect touches only the text,
    // never the pane background.
    auto *textWrap = new QWidget;
    textWrap->setStyleSheet("background: transparent;");
    auto *navV = new QVBoxLayout(textWrap);
    navV->setContentsMargins(12, 14, 8, 10);
    navV->setSpacing(0);
    outerV->addWidget(textWrap);

    auto *controlHome = new QPushButton("Control Panel Home");
    controlHome->setCursor(Qt::PointingHandCursor);
    controlHome->setFlat(true);
    controlHome->setAccessibleName(tr("Control Panel Home"));
    controlHome->setStyleSheet(
        // padding-left 12 matches the category links' contentsMargins(12, …) so
        // "Control Panel Home" lines up with the rest of the sidebar options.
        "QPushButton { border: none; background: transparent; color: #000000;"
        " text-align: left; padding: 0 0 0 12; font-size: 9pt; }"
        "QPushButton:hover { color: #0033AA; }"
        "QPushButton:focus { border: 1px dotted #333333; }"
    );
    QObject::connect(controlHome, &QPushButton::clicked, this,
                     &MainWindow::navigateHome, Qt::QueuedConnection);
    navV->addWidget(controlHome);
    navV->addSpacing(16);

    clip->setWidget(pane);
    return { clip, textWrap, navV };
}

QScrollArea *MainWindow::buildNavSidebar(const QString &currentCategory)
{
    // Width 0: buildCategoryPage animates the clip open to 195px.
    Sidebar bar = buildSidebarShell(0);

    for (const QString &cat : navOrder()) {
        if (cat == currentCategory) {
            auto *row = new QHBoxLayout;
            row->setContentsMargins(0, 3, 0, 3);
            row->setSpacing(4);
            auto *bullet = new QLabel("•");
            bullet->setStyleSheet("color: #000000; font-size: 9pt; background: transparent;");
            row->addWidget(bullet, 0, Qt::AlignTop);
            auto *label = new QLabel(cat);
            QFont bf = label->font();
            bf.setPointSize(9);
            bf.setBold(true);
            label->setFont(bf);
            label->setWordWrap(true);
            label->setStyleSheet("color: #000000; background: transparent;");
            row->addWidget(label, 1);
            bar.navV->addLayout(row);
        } else {
            auto *link = new LinkLabel(Branding::brand(cat));
            QFont lf = link->font();
            lf.setPointSize(9);
            link->setFont(lf);
            link->setWordWrap(true);
            link->setCursor(Qt::PointingHandCursor);
            link->setContentsMargins(12, 3, 0, 3);
            link->setStyleSheet(
                "QLabel { color: #000000; background: transparent; }"
                "QLabel:hover { color: #0033AA; }"
            );
            connect(link, &LinkLabel::clicked, this, [this, cat]() { navigateTo(cat); }, Qt::QueuedConnection);
            bar.navV->addWidget(link);
        }
    }
    bar.navV->addStretch(1);

    // Static sidebar (shown at full opacity). The effect is kept so that
    // navigating into a subpage can fade this text out first.
    m_sidebarTextEffect = new QGraphicsOpacityEffect(bar.textWrap);
    bar.textWrap->setGraphicsEffect(m_sidebarTextEffect);
    m_sidebarTextEffect->setOpacity(1.0);

    return bar.clip;
}

void MainWindow::registerActionLink(LinkLabel *label, ActionId action)
{
    const auto &definition = ActionCatalog::get(action);
    const auto availability = ActionCatalog::availability(action);
    label->setProperty("actionId", static_cast<int>(action));
    label->setEnabled(availability.available);
    label->setAccessibleDescription(availability.available ? definition.description : availability.reason);
    label->setToolTip(availability.available ? definition.description : availability.reason);
    connect(label, &LinkLabel::clicked, this, [this, action]() { activateAction(action); }, Qt::QueuedConnection);
}

void MainWindow::registerLinkTarget(LinkLabel *label, const LinkTarget &target)
{
    const ActionId action = ActionCatalog::idForTarget(target);
    if (action != ActionId::None) { registerActionLink(label, action); return; }
    const auto availability = ActionCatalog::availability(target);
    label->setEnabled(availability.available);
    if (!availability.available) {
        label->setToolTip(availability.reason);
        label->setAccessibleDescription(label->toolTip());
    }
    connect(label, &LinkLabel::clicked, this, [this, target]() { activateTarget(target); }, Qt::QueuedConnection);
}

void MainWindow::activateAction(ActionId action)
{
    const auto availability = ActionCatalog::availability(action);
    if (!availability.available) return;
    activateTarget(ActionCatalog::get(action).target);
}

void MainWindow::activateTarget(const LinkTarget &target)
{
    switch (target.kind) {
    case LinkTarget::Home: navigateHome(); break;
    case LinkTarget::Page: navigateTo(PageRegistry::pathFor(target.page)); break;
    case LinkTarget::Command:
        requestNavigation([this, command = target.command]() { launchDetached(this, command); });
        break;
    case LinkTarget::Applet:
        requestNavigation([this, applet = target.applet]() { openApplet(applet); });
        break;
    case LinkTarget::Callback:
        if (target.callback == CallbackId::CheckUpdates) {
            requestNavigation([this]() {
                pushHistory(kUpdatePath);
                showEntry(kUpdatePath);
                UpdateService::instance()->checkForUpdates();
            });
        }
        break;
    case LinkTarget::None: break;
    }
}

QScrollArea *MainWindow::buildSubpageSidebar(const QList<SidebarLink> &links,
                                              const QList<SidebarLink> &seeAlso)
{
    Sidebar bar = buildSidebarShell(195);

    auto addLink = [&](const SidebarLink &sl) {
        auto *l = new LinkLabel(Branding::brand(sl.text));
        l->setWordWrap(true);
        l->setContentsMargins(12, 3, 0, 3);
        l->setStyleSheet("QLabel { color: #000000; background: transparent; } QLabel:hover { color: #0033AA; } QLabel:disabled { color: #777777; }");
        registerLinkTarget(l, sl.target);
        bar.navV->addWidget(l);
    };

    for (const SidebarLink &sl : links)
        addLink(sl);

    bar.navV->addStretch(1);

    if (!seeAlso.isEmpty()) {
        auto *sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet("color: #B8C4D8;");
        bar.navV->addWidget(sep);
        bar.navV->addSpacing(4);

        auto *seeAlsoLabel = new QLabel("See also");
        QFont f = seeAlsoLabel->font();
        f.setPointSize(8);
        seeAlsoLabel->setFont(f);
        seeAlsoLabel->setContentsMargins(12, 0, 0, 0);
        seeAlsoLabel->setStyleSheet("color: #666666; background: transparent;");
        bar.navV->addWidget(seeAlsoLabel);

        for (const SidebarLink &sl : seeAlso)
            addLink(sl);
    }

    // Subpage sidebars are full width from the start and fade their text in.
    auto *fadeEffect = new QGraphicsOpacityEffect(bar.textWrap);
    bar.textWrap->setGraphicsEffect(fadeEffect);
    fadeEffect->setOpacity(0.0);
    auto *fadeAnim = new QPropertyAnimation(fadeEffect, "opacity", bar.clip);
    fadeAnim->setStartValue(0.0);
    fadeAnim->setEndValue(1.0);
    fadeAnim->setDuration(2000);
    fadeAnim->setEasingCurve(QEasingCurve::OutCubic);
    QTimer::singleShot(0, bar.clip, [fadeAnim]() { fadeAnim->start(); });

    return bar.clip;
}

QWidget *MainWindow::buildCategoryPage(const QString &currentCategory)
{
    auto *page = new QWidget;
    page->setObjectName("categoryPage");
    page->setStyleSheet("#categoryPage { background: #FFFFFF; }");
    auto *root = new QHBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *sidebarClip = buildNavSidebar(currentCategory);
    root->addWidget(sidebarClip);

    // ---- Right content pane ----------------------------------------------
    auto *contentWrap = new QWidget;
    contentWrap->setObjectName("categoryContent");
    contentWrap->setStyleSheet("#categoryContent { background: #FFFFFF; }");
    auto *contentV = new QVBoxLayout(contentWrap);
    contentV->setContentsMargins(18, 14, 18, 14);
    contentV->setSpacing(0);

    auto makeSep = []() {
        auto *s = new QLabel("|");
        QFont f = s->font();
        f.setPointSize(9);
        s->setFont(f);
        s->setStyleSheet("color: #B0B0B0;");
        return s;
    };
    auto makeTask = [this](ActionId action) {
        auto *l = new LinkLabel(Branding::brand(ActionCatalog::get(action).title));
        registerActionLink(l, action);
        return l;
    };

    for (const DetailGroup &group : *detailGroupsFor(currentCategory)) {
        const auto &definition = ActionCatalog::get(group.action);
        auto *groupRow = new QHBoxLayout;
        groupRow->setContentsMargins(0, 0, 0, 12);
        groupRow->setSpacing(12);

        auto *iconLabel = new QLabel;
        iconLabel->setPixmap(resolveIcon(definition.iconName).pixmap(32, 32));
        iconLabel->setFixedSize(32, 32);
        groupRow->addWidget(iconLabel, 0, Qt::AlignTop);

        auto *textBlock = new QVBoxLayout;
        textBlock->setContentsMargins(0, 0, 0, 0);
        textBlock->setSpacing(2);

        auto *title = new LinkLabel(Branding::brand(definition.title));
        QFont tf = title->font();
        tf.setPointSize(11);
        title->setFont(tf);
        title->setCursor(Qt::PointingHandCursor);
        title->setStyleSheet(
            "QLabel { color: #1E7B1E; }"
            "QLabel:hover { color: #145214; }"
        );
        registerActionLink(title, group.action);
        textBlock->addWidget(title);

        for (const QList<ActionId> &line : group.lines) {
            auto *lineRow = new QHBoxLayout;
            lineRow->setContentsMargins(0, 0, 0, 0);
            lineRow->setSpacing(6);
            for (int i = 0; i < line.size(); ++i) {
                if (i > 0)
                    lineRow->addWidget(makeSep());
                lineRow->addWidget(makeTask(line[i]));
            }
            lineRow->addStretch(1);
            textBlock->addLayout(lineRow);
        }

        groupRow->addLayout(textBlock, 1);
        contentV->addLayout(groupRow);
    }

    contentV->addStretch(1);
    root->addWidget(contentWrap, 1);

    // sidebarClip slides from 0 to 195; sidebar inside stays full-width so text
    // never reflows. The content pane rides rightward naturally as the clip grows.
    auto *sidebarAnim = new QVariantAnimation(page);
    sidebarAnim->setStartValue(0);
    sidebarAnim->setEndValue(195);
    sidebarAnim->setDuration(250);
    sidebarAnim->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(sidebarAnim, &QVariantAnimation::valueChanged, sidebarClip,
        [sidebarClip](const QVariant &v) { sidebarClip->setFixedWidth(v.toInt()); });

    // Content fades in over the same span.
    auto *fadeEffect = new QGraphicsOpacityEffect(contentWrap);
    contentWrap->setGraphicsEffect(fadeEffect);
    fadeEffect->setOpacity(0.0);
    auto *contentAnim = new QPropertyAnimation(fadeEffect, "opacity", page);
    contentAnim->setStartValue(0.0);
    contentAnim->setEndValue(1.0);
    contentAnim->setDuration(260);
    contentAnim->setEasingCurve(QEasingCurve::OutCubic);

    QTimer::singleShot(0, page, [sidebarAnim, contentAnim]() {
        sidebarAnim->start();
        contentAnim->start();
    });

    return page;
}

