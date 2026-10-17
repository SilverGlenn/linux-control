#include "LinuxUpdatePage.h"
#include "IconHelper.h"
#include "Win7Ui.h"
#include "Branding.h"
#include "../../operations/UpdateService.h"

#include <QScrollArea>
#include <QMessageBox>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QIcon>
#include <QFont>
#include <QEvent>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QTimer>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QDateTime>
#include <QLocale>

QList<SidebarLink> LinuxUpdatePage::sidebarLinks()
{
    // The typed callback shares the action catalog used by search and views.
    return {
        Nav::callback("Check for updates", CallbackId::CheckUpdates),
        Nav::plain("Change settings"),
        Nav::plain("View update history"),
        Nav::plain("Restore hidden updates"),
        Nav::plain("Updates: frequently asked questions"),
    };
}

QList<SidebarLink> LinuxUpdatePage::sidebarSeeAlso()
{
    return { Nav::to("Installed Updates", PageId::InstalledUpdates),
             Nav::to("Ubuntu Package Preview", PageId::PackagePreview) };
}

LinuxUpdatePage::LinuxUpdatePage(QScrollArea *sidebar, QWidget *parent)
    : QWidget(parent)
{
    // ID-scoped backgrounds throughout: a declaration-only stylesheet acts as
    // `* { ... }`, matching every descendant and forcing them (scroll bars
    // included) into stylesheet rendering instead of the platform style.
    setObjectName("luPage");
    setStyleSheet("#luPage { background: #FFFFFF; }");
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // The page hosts two views in a stack: the update status view (index 0,
    // built below) and an on-demand "Select updates to install" view.
    m_stack = new QStackedWidget;
    root->addWidget(m_stack);

    auto *statusView = new QWidget;
    statusView->setObjectName("luStatusView");
    statusView->setStyleSheet("#luStatusView { background: #FFFFFF; }");
    auto *statusRoot = new QHBoxLayout(statusView);
    statusRoot->setContentsMargins(0, 0, 0, 0);
    statusRoot->setSpacing(0);
    statusRoot->addWidget(sidebar);

    auto *contentWrap = new QWidget;
    contentWrap->setObjectName("luContentWrap");
    contentWrap->setStyleSheet("#luContentWrap { background: #FFFFFF; }");
    auto *contentV = new QVBoxLayout(contentWrap);
    contentV->setContentsMargins(28, 20, 28, 20);
    contentV->setSpacing(0);

    // Page title
    auto *pageTitle = new QLabel(Branding::brand("Linux Update"));
    {
        QFont f = pageTitle->font();
        f.setPointSize(11);
        pageTitle->setFont(f);
    }
    pageTitle->setStyleSheet("color: #003399;");
    contentV->addWidget(pageTitle);
    contentV->addSpacing(16);

    // Status box
    m_statusBox = new QFrame;
    m_statusBox->setObjectName("statusBox");
    m_statusBox->setMaximumWidth(620);

    auto *statusH = new QHBoxLayout(m_statusBox);
    statusH->setContentsMargins(12, 12, 12, 12);
    statusH->setSpacing(12);

    m_iconLabel = new QLabel;
    m_iconLabel->setFixedSize(48, 48);
    m_iconLabel->setStyleSheet("background: transparent; border: none;");
    statusH->addWidget(m_iconLabel, 0, Qt::AlignTop);

    // Centre text column
    auto *textCol = new QVBoxLayout;
    textCol->setContentsMargins(0, 0, 0, 0);
    textCol->setSpacing(4);

    m_titleLabel = new QLabel;
    {
        QFont f = m_titleLabel->font();
        f.setPointSize(11);
        m_titleLabel->setFont(f);
    }
    m_titleLabel->setWordWrap(true);
    m_titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_titleLabel->setStyleSheet("color: #000000; background: transparent; border: none;");
    textCol->addWidget(m_titleLabel);

    // Subtitle row: link text (50%) | separator | selected count (50%).
    m_subRowWidget = new QWidget;
    m_subRowWidget->setStyleSheet("background: transparent;");
    auto *subRow = new QHBoxLayout(m_subRowWidget);
    // A small top margin leaves just a little space above the subtitle; the
    // progress bar sits directly above it when an install/download is running.
    subRow->setContentsMargins(0, 2, 0, 0);
    subRow->setSpacing(14);

    // Hyperlink lines reuse the same styling and signal wiring. linkHovered
    // toggles the underline so links are plain until the pointer is over them.
    auto makeLinkLabel = [this]() {
        auto *lbl = new QLabel;
        QFont f = lbl->font();
        f.setPointSize(9);
        lbl->setFont(f);
        lbl->setWordWrap(true);
        lbl->setTextFormat(Qt::RichText);
        lbl->setOpenExternalLinks(false);
        lbl->setStyleSheet("color: #444444; background: transparent; border: none;");
        connect(lbl, &QLabel::linkActivated, this, &LinuxUpdatePage::openUpdateList);
        connect(lbl, &QLabel::linkHovered, this, [this, lbl](const QString &link) {
            setLinkLabel(lbl, lbl->property("linkText").toString(), !link.isEmpty());
        });
        return lbl;
    };

    m_subLabel  = makeLinkLabel();
    m_subLabel2 = makeLinkLabel();
    m_subLabel2->hide();

    // Stack the two hyperlink lines vertically, tightly (near single-spacing)
    // to match the reference layout.
    auto *linksCol = new QVBoxLayout;
    linksCol->setContentsMargins(0, 0, 0, 0);
    linksCol->setSpacing(2);
    linksCol->addWidget(m_subLabel);
    linksCol->addWidget(m_subLabel2);
    subRow->addLayout(linksCol, 1);

    m_subSep = new QFrame;
    m_subSep->setFixedWidth(1);
    m_subSep->setFixedHeight(34);
    m_subSep->setStyleSheet("QFrame { background: #C0C0C0; border: none; }");
    m_subSep->hide();
    subRow->addWidget(m_subSep, 0, Qt::AlignVCenter);

    m_selectedLabel = new QLabel;
    {
        QFont f = m_selectedLabel->font();
        f.setPointSize(9);
        m_selectedLabel->setFont(f);
    }
    m_selectedLabel->setWordWrap(true);
    m_selectedLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_selectedLabel->setStyleSheet("color: #333333; background: transparent; border: none; font-weight: bold;");
    m_selectedLabel->hide();
    subRow->addWidget(m_selectedLabel, 1);

    textCol->addWidget(m_subRowWidget);

    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 0);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(18);
    m_progressBar->setMaximumWidth(160);
    m_progressBar->setStyleSheet("QProgressBar::chunk { width: 80px; }");
    m_progressBar->hide();
    // Sit the bar between the title (index 0) and the subtitle row (index 1).
    textCol->insertWidget(1, m_progressBar);

    m_progressText = new QLabel;
    {
        QFont f = m_progressText->font();
        f.setPointSize(8);
        m_progressText->setFont(f);
    }
    m_progressText->setStyleSheet("color: #555555; background: transparent; border: none;");
    m_progressText->hide();
    textCol->addWidget(m_progressText);

    // Install button row, sits below the subtitle inside textCol, right-aligned.
    // Showing it naturally expands the status box height.
    m_rightWidget = new QWidget;
    m_rightWidget->setStyleSheet("background: transparent;");
    auto *btnRow = new QHBoxLayout(m_rightWidget);
    btnRow->setContentsMargins(0, 6, 0, 0);
    btnRow->setSpacing(0);
    btnRow->addStretch(1);

    m_installBtn = new QPushButton("Install updates");
    m_installBtn->setStyleSheet("QPushButton { padding-left: 12px; padding-right: 12px; }");
    connect(m_installBtn, &QPushButton::clicked, this, &LinuxUpdatePage::installUpdates);
    btnRow->addWidget(m_installBtn);

    m_rightWidget->hide();
    textCol->addWidget(m_rightWidget);

    statusH->addLayout(textCol, 1);

    contentV->addWidget(m_statusBox);
    contentV->addSpacing(20);

    // Info rows
    auto makeInfoRow = [&](const QString &key, QLabel **out) {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);

        auto *left = new QLabel(key);
        {
            QFont f = left->font();
            f.setPointSize(9);
            left->setFont(f);
        }
        left->setFixedWidth(215);
        left->setStyleSheet("color: #444444;");
        row->addWidget(left);

        auto *right = new QLabel;
        {
            QFont f = right->font();
            f.setPointSize(9);
            right->setFont(f);
        }
        right->setTextFormat(Qt::RichText);
        right->setStyleSheet("color: #000000;");
        row->addWidget(right);
        row->addStretch(1);
        contentV->addLayout(row);
        contentV->addSpacing(3);
        if (out) *out = right;
    };

    makeInfoRow("Most recent check for updates:", &m_lastCheckVal);
    makeInfoRow("Updates were installed:", &m_lastInstallVal);

    QLabel *recvRight = nullptr;
    makeInfoRow("You receive updates:", &recvRight);

    contentV->addSpacing(16);

    // FOSS hint box
    auto *fossBox = new QFrame;
    fossBox->setFrameShape(QFrame::StyledPanel);
    fossBox->setStyleSheet("QFrame { background: #F8F8FF; border: 1px solid #C8C8E0; }");
    fossBox->setMaximumWidth(620);
    {
        auto *h = new QHBoxLayout(fossBox);
        h->setContentsMargins(8, 10, 8, 10);
        auto *lbl = new QLabel(
            "Find out more about free software from the Arch User Repository (AUR).");
        lbl->setTextFormat(Qt::PlainText);
        lbl->setStyleSheet("border: none; background: transparent;");
        {
            QFont f = lbl->font();
            f.setPointSize(9);
            lbl->setFont(f);
        }
        h->addWidget(lbl);
    }
    contentV->addWidget(fossBox);
    contentV->addStretch(1);

    statusRoot->addWidget(contentWrap, 1);
    m_stack->addWidget(statusView);

    // Fade in
    auto *fadeEffect = new QGraphicsOpacityEffect(contentWrap);
    contentWrap->setGraphicsEffect(fadeEffect);
    fadeEffect->setOpacity(0.0);
    auto *anim = new QPropertyAnimation(fadeEffect, "opacity", this);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setDuration(260);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    QTimer::singleShot(0, this, [anim]() { anim->start(); });

    auto *service = UpdateService::instance();
    service->refreshAvailability();
    recvRight->setText(service->state().pacmanAvailable
        ? Branding::brand("For Linux and other products from pacman and the AUR")
        : QStringLiteral("Package update checks require pacman (Arch-based distributions)."));
    connect(service, &UpdateService::changed, this, &LinuxUpdatePage::syncFromService);
    syncFromService();
    // No auto-check, user must click "Check for updates" in the sidebar.
}

// Status box appearance
// Restyle the status box. An empty leftColor gives the plain white box shown
// while a check or download is running; otherwise a 20px stripe in the given
// colour marks the state (yellow for action needed, green for OK, red for error).
void LinuxUpdatePage::applyStatusBorder(const QString &leftColor, const QString &bg)
{
    const QString accent = leftColor.isEmpty()
        ? QString()
        : QString("  border-left: 20px solid %1;").arg(leftColor);
    m_statusBox->setStyleSheet(QString(
        "QFrame#statusBox {"
        "  background: %1;"
        "  border: 1px solid #CCCCCC;"
        "%2"
        "}"
        "QFrame#statusBox QLabel { background: transparent; border: none; }"
    ).arg(bg, accent));
}

// State setters
// Baseline for the status box: the subtitle row with a single hyperlink line
// visible and every optional widget hidden. Each state setter calls this first,
// then re-shows only the pieces it needs.
void LinuxUpdatePage::resetStatusWidgets()
{
    m_subLabel->setTextFormat(Qt::PlainText);
    m_subLabel2->setTextFormat(Qt::PlainText);
    m_subLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_subLabel2->setTextInteractionFlags(Qt::NoTextInteraction);
    m_subRowWidget->setMaximumHeight(QWIDGETSIZE_MAX);
    m_subRowWidget->show();
    m_subLabel->show();
    m_subLabel2->hide();
    m_subSep->hide();
    m_selectedLabel->hide();
    m_progressBar->hide();
    m_progressText->hide();
    m_rightWidget->hide();
}

void LinuxUpdatePage::setIdleState()
{
    applyStatusBorder("#D4A800");
    resetStatusWidgets();
    m_iconLabel->setPixmap(themeIcon({"security-medium", "view-refresh"}).pixmap(48, 48));
    m_titleLabel->setText("There may be updates available");
    m_subLabel->setText("Click “Check for updates” in the sidebar to search for available updates.");
}

void LinuxUpdatePage::setCheckingState()
{
    applyStatusBorder("");
    resetStatusWidgets();
    m_iconLabel->setPixmap(themeIcon({"system-software-update"}).pixmap(48, 48));
    m_titleLabel->setText("Checking for updates...");
    m_subRowWidget->setMaximumHeight(0);
    m_subRowWidget->hide();
    m_progressBar->setRange(0, 0);
    m_progressBar->show();
}

void LinuxUpdatePage::setNoUpdatesState()
{
    applyStatusBorder("#1DA51D");
    resetStatusWidgets();
    m_iconLabel->setPixmap(themeIcon({"security-high", "emblem-default", "dialog-ok-apply"}).pixmap(48, 48));
    m_titleLabel->setText(Branding::brand("Linux is up to date"));
    m_subLabel->setText("There are no available updates for your computer.");
}

void LinuxUpdatePage::setCheckErrorState(const QString &message)
{
    applyStatusBorder("#CC3300");
    resetStatusWidgets();
    m_iconLabel->setPixmap(themeIcon({"dialog-error", "security-low"}).pixmap(48, 48));
    m_titleLabel->setText("Update status unavailable");
    m_subLabel->setText(message);
    m_installBtn->setEnabled(false);
}

void LinuxUpdatePage::setUpdatesAvailableState()
{
    applyStatusBorder("#D4A800");
    resetStatusWidgets();
    m_iconLabel->setPixmap(themeIcon({"security-medium", "dialog-warning", "emblem-important"}).pixmap(48, 48));

    int nImp = 0, nOpt = 0;
    for (const auto &p : m_pkgs) { if (p.aur) nOpt++; else nImp++; }
    m_titleLabel->setText("Install updates for your computer");

    // Subtitle: important and optional updates as separate hyperlinks on their
    // own lines. Each link opens the select-updates page.
    if (nImp > 0) {
        setLinkLabel(m_subLabel,
                     QString("%1 important update%2 available")
                         .arg(nImp).arg(nImp == 1 ? " is" : "s are"),
                     false);
        m_subLabel->show();
    } else {
        m_subLabel->hide();
    }
    if (!m_aurCheckWarning.isEmpty()) {
        m_subLabel2->setText(m_aurCheckWarning);
        m_subLabel2->setTextFormat(Qt::PlainText);
        m_subLabel2->setTextInteractionFlags(Qt::NoTextInteraction);
        m_subLabel2->show();
    } else if (nOpt > 0) {
        setLinkLabel(m_subLabel2,
                     QString("%1 optional update%2 available")
                         .arg(nOpt).arg(nOpt == 1 ? " is" : "s are"),
                     false);
        m_subLabel2->show();
    }

    QStringList parts;
    if (nImp > 0) parts << QString("%1 important update%2").arg(nImp).arg(nImp == 1 ? "" : "s");
    if (nOpt > 0) parts << QString("%1 optional update%2").arg(nOpt).arg(nOpt == 1 ? "" : "s");
    m_selectedLabel->setText(parts.join(", ") + " selected");
    m_selectedLabel->show();
    m_subSep->show();
    m_rightWidget->show();
    m_installBtn->setEnabled(true);

    int selectedImportant = 0, selectedOptional = 0;
    for (const auto &pkg : m_pkgs) {
        if (!m_selectedPkgNames.contains(pkg.name)) continue;
        if (pkg.aur) ++selectedOptional; else ++selectedImportant;
    }
    parts.clear();
    if (selectedImportant) parts << QString("%1 important update%2").arg(selectedImportant).arg(selectedImportant == 1 ? "" : "s");
    if (selectedOptional) parts << QString("%1 optional update%2").arg(selectedOptional).arg(selectedOptional == 1 ? "" : "s");
    m_selectedLabel->setText(parts.isEmpty() ? "No updates selected" : parts.join(", ") + " selected");
    m_installBtn->setEnabled(m_selectedCount > 0);

}

void LinuxUpdatePage::setDownloadingState()
{
    applyStatusBorder("");
    resetStatusWidgets();
    m_iconLabel->setPixmap(themeIcon({"system-software-update", "view-refresh"}).pixmap(48, 48));
    m_titleLabel->setText("Downloading updates...");
    // Package sizes are not reliably available, so progress is reported as the
    // share of selected updates whose download has started, not as a byte total.
    m_subLabel->setText(QString("Downloading %1 update%2 (0% complete)")
        .arg(m_selectedCount).arg(m_selectedCount == 1 ? "" : "s"));
    m_progressBar->setRange(0, 0);
    m_progressBar->show();
}

void LinuxUpdatePage::setInstallingPhaseState()
{
    m_titleLabel->setText("Installing updates...");
    m_subLabel->setText("Installing updates...");
    m_progressBar->setRange(0, 0);
}

void LinuxUpdatePage::setDoneState(bool ok)
{
    applyStatusBorder(ok ? "#1DA51D" : "#CC3300");
    resetStatusWidgets();
    m_iconLabel->setPixmap(ok
        ? themeIcon({"security-high", "dialog-ok-apply"}).pixmap(48, 48)
        : themeIcon({"dialog-error", "security-low"}).pixmap(48, 48));
    m_titleLabel->setText(ok
        ? "The updates were installed successfully."
        : "Some updates failed to install.");
    if (ok) {
        // Anything that was available but not part of this selection is still
        // outstanding -- count both repo and AUR updates, not just optional ones.
        int remaining = 0;
        for (const auto &p : m_pkgs)
            if (!m_selectedPkgNames.contains(p.name)) remaining++;
        if (remaining > 0)
            m_subLabel->setText("There are still updates available for your computer.");
        else
            m_subLabel->setText("Your system is up to date.");
    } else {
        m_subLabel->setText("Check your internet connection and try again, or run pacman -Syu in a terminal.");
    }
}


// Info rows
QString LinuxUpdatePage::formatStamp(const QDateTime &dt)
{
    if (!dt.isValid())
        return "Never";
    const QLocale loc = QLocale::system();
    const QString time = loc.toString(dt.time(), QLocale::ShortFormat);
    if (dt.date() == QDate::currentDate())
        return QString("Today at %1").arg(time);
    if (dt.date() == QDate::currentDate().addDays(-1))
        return QString("Yesterday at %1").arg(time);
    return QString("%1 at %2").arg(loc.toString(dt.date(), QLocale::ShortFormat), time);
}

void LinuxUpdatePage::updateInfoRows()
{
    m_lastCheckVal->setText(formatStamp(m_lastCheck));

    if (m_everInstalled) {
        QString suffix = m_lastInstallOk ? "" : " (Failed).";
        m_lastInstallVal->setText(
            QString("%1%2  <a href='#' style='color:#1F4E99;'>View update history</a>")
            .arg(formatStamp(m_lastInstall), suffix));
    } else {
        m_lastInstallVal->setText("Never");
    }

}

// Update list dialog
void LinuxUpdatePage::setLinkLabel(QLabel *label, const QString &text, bool underline)
{
    label->setTextFormat(Qt::RichText);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setProperty("linkText", text);
    label->setText(QString("<a href='list' style='color:#1F4E99; text-decoration:%1;'>%2</a>")
                   .arg(underline ? "underline" : "none", text));
}

void LinuxUpdatePage::openUpdateList()
{
    // The update-selection UI is now an in-window page. Ask MainWindow to
    // navigate to it; it calls showSelectView() on this same instance so the
    // checked-update state lives here and survives the view switch.
    emit selectUpdatesRequested();
}

// Select-updates page
void LinuxUpdatePage::showStatusView()
{
    if (m_stack)
        m_stack->setCurrentIndex(0);
}

void LinuxUpdatePage::showSelectView()
{
    // Rebuild the select page from the current package list each time it opens
    // so it reflects the latest check.
    if (m_selectView) {
        m_stack->removeWidget(m_selectView);
        m_selectView->deleteLater();
        m_selectView = nullptr;
    }
    m_selectView = buildSelectView();
    m_stack->addWidget(m_selectView);
    m_stack->setCurrentWidget(m_selectView);

    // The tree header can still cache a too-tall size hint that it computed
    // before its section stylesheet was polished on first show. Clicking a
    // category used to fix this by accident (rebuilding the table reset the
    // header). Re-polish the header once, now that the view is visible, so it
    // settles at the correct height on its own.
    if (m_selTree) {
        QHeaderView *hdr = m_selTree->header();
        QMetaObject::invokeMethod(hdr, [hdr]() {
            hdr->setStyleSheet(hdr->styleSheet());
        }, Qt::QueuedConnection);
    }
}

QWidget *LinuxUpdatePage::buildSelectView()
{
    // Sort important (pacman) first, optional (AUR) last.
    m_selPkgs.clear();
    for (const auto &p : m_pkgs) if (!p.aur) m_selPkgs << p;
    for (const auto &p : m_pkgs) if ( p.aur) m_selPkgs << p;

    m_selChecked = UpdateService::instance()->state().checkedNames;

    int nImp = 0, nOpt = 0;
    for (const auto &p : m_selPkgs) { if (p.aur) nOpt++; else nImp++; }
    m_selFilter = (nImp > 0) ? 0 : 1;

    auto *view = new QWidget;
    view->setObjectName("luSelectView");
    view->setStyleSheet("#luSelectView { background: #FFFFFF; }");
    auto *root = new QVBoxLayout(view);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header
    auto *headerBar = new QWidget;
    headerBar->setObjectName("luSelectHeader");
    headerBar->setStyleSheet("#luSelectHeader { background: #FFFFFF; }");
    auto *hh = new QHBoxLayout(headerBar);
    hh->setContentsMargins(28, 18, 28, 12);
    auto *hdr = new QLabel("Select the updates you want to install");
    {
        QFont f = hdr->font();
        f.setPointSize(12);
        hdr->setFont(f);
    }
    hdr->setStyleSheet("color: #003399;");
    hh->addWidget(hdr);
    hh->addStretch();
    root->addWidget(headerBar);

    auto *topSep = new QFrame;
    topSep->setFrameShape(QFrame::HLine);
    topSep->setStyleSheet("color: #D9D9D9;");
    root->addWidget(topSep);

    // Body: category sidebar | table | details
    auto *body = new QWidget;
    body->setObjectName("luSelectBody");
    body->setStyleSheet("#luSelectBody { background: #FFFFFF; }");
    auto *bodyH = new QHBoxLayout(body);
    bodyH->setContentsMargins(0, 0, 0, 0);
    bodyH->setSpacing(0);

    // Category sidebar (Important / Optional).
    auto *catBar = new QFrame;
    catBar->setObjectName("catBar");
    catBar->setFixedWidth(150);
    catBar->setStyleSheet("#catBar { background: #F1F4F9; border-right: 1px solid #DCE0E8; }");
    auto *catV = new QVBoxLayout(catBar);
    catV->setContentsMargins(8, 14, 8, 10);
    catV->setSpacing(2);

    auto makeCat = [this](const QString &text) {
        auto *b = new QPushButton(text);
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        QFont f = b->font();
        f.setPointSize(9);
        b->setFont(f);
        return b;
    };
    m_selImpItem = makeCat(QString("Important (%1)").arg(nImp));
    m_selOptItem = makeCat(QString("Optional (%1)").arg(nOpt));
    connect(m_selImpItem, &QPushButton::clicked, this, [this]() { setSelectFilter(0); });
    connect(m_selOptItem, &QPushButton::clicked, this, [this]() { setSelectFilter(1); });
    catV->addWidget(m_selImpItem);
    catV->addWidget(m_selOptItem);
    catV->addStretch(1);
    bodyH->addWidget(catBar);

    // Update tree: a "Linux (N)" group node with one update per child row.
    // QTreeWidget gives us the native header and a collapsible group whose
    // auto-tristate checkbox drives the whole group (no manual master logic).
    m_selTree = new QTreeWidget;
    m_selTree->setColumnCount(2);
    m_selTree->setHeaderLabels({ "Name", "Size" });
    Win7::configureListTree(m_selTree);
    // Unlike the flat ribbon lists, this tree keeps its expandable group node.
    m_selTree->setRootIsDecorated(true);
    m_selTree->setIndentation(20);
    m_selTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_selTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    connect(m_selTree, &QTreeWidget::itemChanged, this,
        [this](QTreeWidgetItem *it, int col) {
            if (m_selUpdating || col != 0) return;
            const QString name = it->data(0, Qt::UserRole).toString();
            if (name.isEmpty()) {
                // Group node: auto-tristate has cascaded to the children, so
                // mirror their states into the checked set.
                for (int i = 0; i < it->childCount(); ++i) {
                    auto *c = it->child(i);
                    const QString n = c->data(0, Qt::UserRole).toString();
                    if (c->checkState(0) == Qt::Checked) m_selChecked.insert(n);
                    else                                 m_selChecked.remove(n);
                }
            } else {
                if (it->checkState(0) == Qt::Checked) m_selChecked.insert(name);
                else                                  m_selChecked.remove(name);
            }
            UpdateService::instance()->setCheckedNames(m_selChecked);
            updateSelectCount();
        });

    connect(m_selTree, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem *cur, QTreeWidgetItem *) {
            if (!cur) return;
            const QString name = cur->data(0, Qt::UserRole).toString();
            if (name.isEmpty()) return;   // group node
            const PkgInfo *pkg = nullptr;
            for (const auto &p : m_selPkgs) if (p.name == name) { pkg = &p; break; }
            if (!pkg) return;
            m_selDetailTitle->setText(pkg->name);
            m_selDetailBody->setText(
                QString("<b>Update:</b> %1 → %2<br><br>"
                        "For more information run:<br>"
                        "<tt>pacman -Si %3</tt>")
                .arg(pkg->oldVer, pkg->newVer, pkg->name));
            m_selDetailNote->show();
        });

    bodyH->addWidget(m_selTree, 1);

    // Details panel.
    auto *detail = new QWidget;
    detail->setObjectName("luSelectDetail");
    detail->setFixedWidth(300);
    detail->setStyleSheet(
        "#luSelectDetail { background: #FFFFFF; border-left: 1px solid #E0E0E0; }");
    auto *detV = new QVBoxLayout(detail);
    detV->setContentsMargins(12, 12, 12, 12);
    detV->setSpacing(8);

    m_selDetailTitle = new QLabel;
    {
        QFont f = m_selDetailTitle->font();
        f.setPointSize(9);
        f.setBold(true);
        m_selDetailTitle->setFont(f);
    }
    m_selDetailTitle->setWordWrap(true);
    m_selDetailTitle->setStyleSheet("color: #000000; border: none;");
    detV->addWidget(m_selDetailTitle);

    m_selDetailBody = new QLabel("Select a package from the list to see details.");
    {
        QFont f = m_selDetailBody->font();
        f.setPointSize(9);
        m_selDetailBody->setFont(f);
    }
    m_selDetailBody->setWordWrap(true);
    m_selDetailBody->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_selDetailBody->setStyleSheet("color: #333333; border: none;");
    detV->addWidget(m_selDetailBody, 1);

    m_selDetailNote = new QLabel(
        "\xe2\x9a\xa0 You may need to restart your computer after installing this update.");
    {
        QFont f = m_selDetailNote->font();
        f.setPointSize(8);
        m_selDetailNote->setFont(f);
    }
    m_selDetailNote->setWordWrap(true);
    m_selDetailNote->setStyleSheet("color: #555555; border: none;");
    m_selDetailNote->hide();
    detV->addWidget(m_selDetailNote);

    bodyH->addWidget(detail);
    root->addWidget(body, 1);

    // Footer: selected-count summary + OK / Cancel.
    auto *botSep = new QFrame;
    botSep->setFrameShape(QFrame::HLine);
    botSep->setStyleSheet("color: #D9D9D9;");
    root->addWidget(botSep);

    auto *footer = new QWidget;
    footer->setObjectName("luSelectFooter");
    footer->setStyleSheet("#luSelectFooter { background: #F4F4F4; }");
    auto *fh = new QHBoxLayout(footer);
    fh->setContentsMargins(14, 8, 14, 8);
    fh->setSpacing(8);

    m_selCountLabel = new QLabel;
    {
        QFont f = m_selCountLabel->font();
        f.setPointSize(9);
        m_selCountLabel->setFont(f);
    }
    m_selCountLabel->setStyleSheet("color: #333333; border: none;");
    fh->addWidget(m_selCountLabel, 1);

    auto *okBtn     = new QPushButton("OK");
    auto *cancelBtn = new QPushButton("Cancel");
    okBtn->setDefault(true);
    okBtn->setMinimumWidth(80);
    cancelBtn->setMinimumWidth(80);
    fh->addWidget(okBtn);
    fh->addWidget(cancelBtn);
    root->addWidget(footer);

    connect(okBtn, &QPushButton::clicked, this, [this]() {
        applySelection();
        emit selectViewClosed();
    });
    connect(cancelBtn, &QPushButton::clicked, this, [this]() {
        const auto &selected = UpdateService::instance()->state().selectedNames;
        UpdateService::instance()->setCheckedNames(QSet<QString>(selected.begin(), selected.end()));
        emit selectViewClosed();
    });

    setSelectFilter(m_selFilter);   // highlights the active category + fills table
    return view;
}

void LinuxUpdatePage::setSelectFilter(int filter)
{
    m_selFilter = filter;

    const QString base =
        "QPushButton { text-align: left; border: 1px solid transparent;"
        " padding: 4px 6px; background: transparent; color: #000000; }"
        "QPushButton:hover { color: #0033AA; }";
    const QString sel =
        "QPushButton { text-align: left; border: 1px solid #B8CDEA;"
        " padding: 4px 6px; background: #D6E4F5; color: #000000; }";
    if (m_selImpItem) m_selImpItem->setStyleSheet(filter == 0 ? sel : base);
    if (m_selOptItem) m_selOptItem->setStyleSheet(filter == 1 ? sel : base);

    populateSelectTable();
    updateSelectCount();
}

void LinuxUpdatePage::populateSelectTable()
{
    if (!m_selTree) return;
    m_selUpdating = true;
    m_selTree->clear();

    QList<PkgInfo> filtered;
    for (const auto &p : m_selPkgs) {
        const bool inFilter = (m_selFilter == 0) ? !p.aur : p.aur;
        if (inFilter) filtered << p;
    }

    // Group node. An empty UserRole marks it as the group (vs. a package row).
    // ItemIsAutoTristate makes its checkbox reflect and drive the children.
    auto *group = new QTreeWidgetItem(m_selTree);
    group->setText(0, QStringLiteral("%1 (%2)")
                          .arg(Branding::os())
                          .arg(filtered.size()));
    {
        QFont f = group->font(0);
        f.setBold(true);
        group->setFont(0, f);
    }
    group->setData(0, Qt::UserRole, QString());
    group->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled
                    | Qt::ItemIsAutoTristate);
    group->setBackground(0, QColor("#E8EBF0"));
    group->setBackground(1, QColor("#E8EBF0"));

    for (const auto &pkg : filtered) {
        auto *child = new QTreeWidgetItem(group);
        child->setText(0, pkg.name);
        child->setText(1, m_pkgSizes.value(pkg.name, QStringLiteral("-")));
        child->setForeground(1, QColor("#555555"));
        child->setData(0, Qt::UserRole, pkg.name);
        child->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled
                        | Qt::ItemIsSelectable);
        child->setCheckState(0, m_selChecked.contains(pkg.name)
                             ? Qt::Checked : Qt::Unchecked);
    }

    group->setExpanded(true);
    m_selUpdating = false;
}

void LinuxUpdatePage::updateSelectCount()
{
    if (!m_selCountLabel) return;
    int selImp = 0, selOpt = 0;
    for (const auto &p : m_selPkgs) {
        if (!m_selChecked.contains(p.name)) continue;
        if (p.aur) ++selOpt; else ++selImp;
    }
    QStringList fp;
    if (selImp > 0) fp << QString("%1 important update%2").arg(selImp).arg(selImp == 1 ? "" : "s");
    if (selOpt > 0) fp << QString("%1 optional update%2").arg(selOpt).arg(selOpt == 1 ? "" : "s");
    m_selCountLabel->setText(fp.isEmpty() ? "Nothing selected" : "Total selected: " + fp.join(", "));
}

void LinuxUpdatePage::applySelection()
{
    UpdateService::instance()->setSelection(m_selChecked);
}

void LinuxUpdatePage::checkForUpdates()
{
    UpdateService::instance()->checkForUpdates();
}

void LinuxUpdatePage::installUpdates()
{
    const QString error = UpdateService::instance()->installUpdates();
    if (!error.isEmpty())
        QMessageBox::information(this, "Package updates", error);
}

void LinuxUpdatePage::syncFromService()
{
    const auto &state = UpdateService::instance()->state();
    m_pkgs = state.packages;
    m_pkgSizes = state.packageSizes;
    m_selectedPkgNames = state.selectedNames;
    m_selectedCount = state.selectedNames.size();
    m_lastCheck = state.lastCheck;
    m_lastInstall = state.lastInstall;
    m_everInstalled = state.everInstalled;
    m_lastInstallOk = state.lastInstallOk;
    m_aurCheckWarning = state.aurWarning;
    updateInfoRows();
    using Phase = UpdateService::Phase;
    switch (state.phase) {
    case Phase::Idle: setIdleState(); break;
    case Phase::Checking: setCheckingState(); break;
    case Phase::Available: setUpdatesAvailableState(); break;
    case Phase::UpToDate: setNoUpdatesState(); break;
    case Phase::Error: setCheckErrorState(state.error); break;
    case Phase::Incomplete:
        applyStatusBorder("#D4A800");
        resetStatusWidgets();
        m_iconLabel->setPixmap(themeIcon({"dialog-warning", "security-medium"}).pixmap(48, 48));
        m_titleLabel->setText("Update status incomplete");
        m_subLabel->setText(state.aurWarning);
        m_subLabel->setTextFormat(Qt::PlainText);
        m_subLabel->setTextInteractionFlags(Qt::NoTextInteraction);
        m_installBtn->setEnabled(false);
        break;
    case Phase::Downloading:
    case Phase::Installing: {
        setDownloadingState();
        const int count = state.phase == Phase::Installing ? state.installCount : state.downloadCount;
        if (state.phase == Phase::Installing) {
            setInstallingPhaseState();
            m_subLabel->setText(QString("Installing update %1 of %2...").arg(count).arg(m_selectedCount));
            if (!state.currentPackage.isEmpty()) {
                m_subLabel2->setText(state.currentPackage);
                m_subLabel2->setTextFormat(Qt::PlainText);
                m_subLabel2->show();
            }
        } else if (count > 0 && m_selectedCount > 0) {
            m_subLabel->setText(QString("Downloading %1 update%2 (%3% complete)")
                .arg(m_selectedCount).arg(m_selectedCount == 1 ? "" : "s").arg(count * 100 / m_selectedCount));
        }
        if (count > 0 && m_selectedCount > 0) {
            m_progressBar->setRange(0, m_selectedCount);
            m_progressBar->setValue(qMin(count, m_selectedCount));
        }
        break;
    }
    case Phase::Done:
        setDoneState(state.lastInstallOk);
        if (!state.lastInstallOk && !state.error.isEmpty()) {
            m_subLabel->setText(state.error);
            m_subLabel->setTextFormat(Qt::PlainText);
        } else if (state.lastInstallOk && !state.aurCheckSucceeded) {
            m_subLabel->setText("Selected updates were installed. AUR update status remains unavailable; check for updates again.");
        }
        break;
    }
    if (m_selTree && m_stack->currentWidget() == m_selectView) {
        // Rebuild the package list when a check finishes while selection is open.
        m_selPkgs.clear();
        for (const auto &pkg : m_pkgs) if (!pkg.aur) m_selPkgs << pkg;
        for (const auto &pkg : m_pkgs) if (pkg.aur) m_selPkgs << pkg;
        m_selChecked = state.checkedNames;
        populateSelectTable();
        updateSelectCount();
    }
}

// Hover underline on link labels
bool LinuxUpdatePage::eventFilter(QObject *watched, QEvent *event)
{
    if (auto *label = qobject_cast<QLabel *>(watched)) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
            QFont font = label->font();
            font.setUnderline(event->type() == QEvent::Enter);
            label->setFont(font);
        }
    }
    return QWidget::eventFilter(watched, event);
}
