#include "ProgramsFeaturesPage.h"
#include "IconHelper.h"
#include "Win7Ui.h"
#include "packages/ProgramsService.h"
#include "operations/OperationManager.h"

#include <QScrollArea>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QFont>
#include <QDialog>
#include <QPushButton>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStyle>
#include <QStandardPaths>
#include <QUrl>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QLocale>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <algorithm>
#include <climits>

// Bytes to "5.63 MB" / "2.70 GB" / "812 KB", mirroring the column in the
// reference screenshot (which lists everything in MB, totals in GB).
static QString humanSize(qint64 bytes)
{
    const double kb = bytes / 1024.0;
    if (kb < 1024.0)
        return QString::number(kb, 'f', 0) + " KB";
    const double mb = kb / 1024.0;
    if (mb < 1024.0)
        return QString::number(mb, 'f', mb < 10.0 ? 2 : (mb < 100.0 ? 1 : 0)) + " MB";
    const double gb = mb / 1024.0;
    return QString::number(gb, 'f', 2) + " GB";
}

// The Size column must sort by the real byte count (stored in SizeSortRole),
// not by the human-readable "5.63 MB" text. All other columns sort lexically,
// which is correct for names, publishers, versions and ISO-formatted dates.
namespace {
constexpr int SizeSortRole = Qt::UserRole + 1;

class ProgramItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override {
        const int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (col == 3) {
            return data(3, SizeSortRole).toLongLong()
                 < other.data(3, SizeSortRole).toLongLong();
        }
        return text(col).localeAwareCompare(other.text(col)) < 0;
    }
};
} // namespace

// Sidebar entries
QList<SidebarLink> ProgramsFeaturesPage::sidebarLinks()
{
    return {
        Nav::to("View installed updates", PageId::InstalledUpdates),
        Nav::to("Ubuntu Package Preview", PageId::PackagePreview),
        Nav::plain("Turn Linux features on or off"),
    };
}

QList<SidebarLink> ProgramsFeaturesPage::sidebarSeeAlso()
{
    return {};
}

// Page
ProgramsFeaturesPage::ProgramsFeaturesPage(QScrollArea *sidebar, QWidget *parent, ProgramsService *service)
    : QWidget(parent), m_service(service ? service : ProgramsService::instance())
{
    auto *contentV = Win7::pageScaffold(this, sidebar, /*bottomMargin=*/0);
    // The list frame (command bar + tree + status strip) is full-bleed, so the
    // column itself carries no horizontal padding; only the header text below is
    // indented, via its own inset sub-layout.
    contentV->setContentsMargins(0, 18, 0, 0);

    // Title + instruction (the only indented block).
    auto *headerV = new QVBoxLayout;
    headerV->setContentsMargins(28, 0, 28, 0);
    headerV->setSpacing(0);
    headerV->addWidget(Win7::pageTitle("Uninstall or change a program"));
    headerV->addSpacing(8);
    headerV->addWidget(Win7::label(
        "Double-click a program to uninstall or repair it. "
        "Operations continue while you visit other pages."));
    contentV->addLayout(headerV);
    contentV->addSpacing(12);
    m_actionStatus = Win7::label(QString(), 9);
    m_actionStatus->setObjectName("programsActionStatus");
    m_actionStatus->setTextFormat(Qt::PlainText);
    m_actionStatus->setWordWrap(true);
    contentV->addWidget(m_actionStatus);
    contentV->addWidget(Win7::hSeparator());

    // "Organize" command bar
    QHBoxLayout *toolH = nullptr;
    auto *toolBar = Win7::commandBar(&toolH);
    toolH->addWidget(Win7::dropdownLabel("Organize"));
    toolH->addStretch(1);
    Win7::addCommandBarIcons(toolH);
    contentV->addWidget(toolBar);

    // Programs tree
    m_tree = new QTreeWidget;
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({ "Name", "Publisher", "Installed On", "Size", "Version" });
    m_tree->setSortingEnabled(true);
    Win7::configureListTree(m_tree);
    for (int c = 0; c < 4; ++c)
        m_tree->header()->setSectionResizeMode(c, QHeaderView::Interactive);
    m_tree->setColumnWidth(0, 300);
    m_tree->setColumnWidth(1, 150);
    m_tree->setColumnWidth(2, 90);
    m_tree->setColumnWidth(3, 70);
    // Double-clicking a program opens the Uninstall/Reinstall dialog.
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &ProgramsFeaturesPage::onProgramActivated);
    contentV->addWidget(m_tree, 1);

    // Status bar: total size + program count
    QHBoxLayout *statusH = nullptr;
    auto *statusBar = Win7::statusPanel(52, &statusH);

    auto *statusIcon = new QLabel;
    statusIcon->setPixmap(themeIcon({"application-vnd.debian.binary-package",
                                     "preferences-system"}).pixmap(42, 42));
    statusH->addWidget(statusIcon);

    auto *statusText = new QVBoxLayout;
    statusText->setContentsMargins(0, 0, 0, 0);
    statusText->setSpacing(0);

    auto *line1 = new QHBoxLayout;
    line1->setContentsMargins(0, 0, 0, 0);
    line1->setSpacing(40);
    m_totalLbl = Win7::label(QString(), 9, "#1F1F1F");
    line1->addWidget(Win7::label("Currently installed programs", 9, "#1F1F1F"));
    line1->addWidget(m_totalLbl);
    line1->addStretch(1);
    statusText->addLayout(line1);

    m_countLbl = Win7::label("0 programs installed", 9, "#555555");
    statusText->addWidget(m_countLbl);

    statusH->addLayout(statusText);
    statusH->addStretch(1);
    contentV->addWidget(statusBar);

    connect(m_service, &ProgramsService::changed, this, &ProgramsFeaturesPage::refreshFromService);
    connect(OperationManager::instance(), &OperationManager::changed,
            this, &ProgramsFeaturesPage::refreshFromService);
    startLoad();
}

void ProgramsFeaturesPage::startLoad()
{
    if (!m_service->isLoading() && !m_service->isActing()) m_service->load(true);
    refreshFromService();
}

void ProgramsFeaturesPage::refreshFromService()
{
    m_actionStatus->setText(m_service->actionStatus());
    m_actionStatus->setVisible(!m_service->actionStatus().isEmpty());
    m_actionStatus->setStyleSheet(m_service->actionFailed()
        ? "color: #CC3300; background: transparent;" : "color: #333333; background: transparent;");
    m_tree->setEnabled(!m_service->isActing()
                      && !OperationManager::instance()->hasActiveMutatingJobs());
    if (m_service->isLoading() && !m_showingLoading) {
        m_showingLoading = true;
        m_tree->clear();
        m_tree->setCursor(Qt::BusyCursor);
        Win7::addTreePlaceholder(m_tree, "Searching for installed programs...");
        m_countLbl->setText("Loading package list");
        m_totalLbl->clear();
    } else if (!m_service->isLoading() && m_service->hasInventory()
               && (m_showingLoading || m_inventoryRevision != m_service->inventoryRevision())) {
        m_showingLoading = false;
        m_inventoryRevision = m_service->inventoryRevision();
        populate(m_service->inventory());
    }
}

void ProgramsFeaturesPage::populate(const LoadResult &result)
{
    m_tree->setCursor(Qt::ArrowCursor);
    m_tree->clear();   // drop the "Searching..." placeholder

    if (!result.error.isEmpty()) {
        Win7::addTreePlaceholder(m_tree, result.error);
        m_countLbl->setText("Package list unavailable");
        m_totalLbl->clear();
        return;
    }

    qint64 totalBytes = 0;
    for (const ProgramInfo &p : result.programs) {
        auto *item = new ProgramItem(m_tree);
        // Prefer the icon named in the program's .desktop file (an absolute path
        // or a theme name), then a theme icon matching the package name, finally
        // a generic package icon.
        QIcon icon;
        if (!p.iconName.isEmpty())
            icon = p.iconName.startsWith('/') ? QIcon(p.iconName)
                                              : QIcon::fromTheme(p.iconName);
        if (icon.isNull()) icon = QIcon::fromTheme(p.name);
        if (icon.isNull()) icon = themeIcon({"exec", "application-x-executable",
                                             "gnome-fs-executable"});
        item->setIcon(0, icon);
        item->setText(0, p.display);
        item->setData(0, Qt::UserRole, p.name);   // package name for the action dialog
        item->setText(1, p.publisher);
        item->setText(2, p.installed.isValid()
                              ? p.installed.toString("yyyy-MM-dd") : QString());
        item->setText(3, humanSize(p.sizeBytes));
        item->setText(4, p.version);
        item->setData(3, SizeSortRole, static_cast<qlonglong>(p.sizeBytes));
        Win7::setItemPointSize(item);
        totalBytes += p.sizeBytes;
    }
    m_tree->sortByColumn(0, Qt::AscendingOrder);

    m_totalLbl->setText("Total size:  " + humanSize(totalBytes));
    m_countLbl->setText(QString("%1 program%2 installed")
                            .arg(result.programs.size())
                            .arg(result.programs.size() == 1 ? "" : "s"));
}

// Uninstall / Reinstall
void ProgramsFeaturesPage::onProgramActivated(QTreeWidgetItem *item, int /*column*/)
{
    if (!item || m_service->isActing()
        || OperationManager::instance()->hasActiveMutatingJobs()) return;
    const QString pkg = item->data(0, Qt::UserRole).toString();
    if (pkg.isEmpty()) return;                 // the "Searching..." placeholder row
    const QString display = item->text(0);

    // A Win7-style confirmation: a white message area (standard warning icon +
    // single-line question) above a slightly darker footer panel holding the
    // Uninstall / Reinstall / Cancel buttons at the bottom-right.
    QDialog dlg(this);
    dlg.setWindowTitle("Programs and Features");
    dlg.setModal(true);
    dlg.setStyleSheet("QDialog { background: #FFFFFF; }");

    auto *outer = new QVBoxLayout(&dlg);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // Lock the dialog to its content's size hint: fixed, non-resizable.
    outer->setSizeConstraint(QLayout::SetFixedSize);

    // Message area (white)
    auto *body = new QWidget;
    body->setStyleSheet("background: #FFFFFF;");
    auto *bodyH = new QHBoxLayout(body);
    bodyH->setContentsMargins(18, 12, 28, 12);
    bodyH->setSpacing(14);

    auto *iconLbl = new QLabel;
    iconLbl->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(32, 32));
    iconLbl->setFixedSize(32, 32);
    bodyH->addWidget(iconLbl, 0, Qt::AlignVCenter);

    auto *msg = new QLabel(
        QString("Do you want to uninstall or repair %1?").arg(display));
    {
        QFont f = msg->font();
        f.setPointSize(9);
        msg->setFont(f);
    }
    msg->setStyleSheet("color: #000000; background: transparent;");
    msg->setMinimumWidth(360);
    bodyH->addWidget(msg, 1, Qt::AlignVCenter);
    outer->addWidget(body);

    // Footer panel (slightly darker, like the Win7 command area)
    auto *footer = new QFrame;
    footer->setObjectName("dlgFooter");
    footer->setStyleSheet(
        "#dlgFooter { background: #F0F0F0; border-top: 1px solid #DFDFDF; }");
    auto *footH = new QHBoxLayout(footer);
    footH->setContentsMargins(12, 8, 12, 8);
    footH->setSpacing(8);
    footH->addStretch(1);
    auto *uninstallBtn = new QPushButton("Uninstall");
    auto *reinstallBtn = new QPushButton("Repair");
    auto *cancelBtn    = new QPushButton("Cancel");
    for (QPushButton *b : { uninstallBtn, reinstallBtn, cancelBtn }) {
        b->setMinimumWidth(75);
        b->setAutoDefault(false);   // don't let the first button claim the default
        footH->addWidget(b);
    }
    // Cancel is both the default (Enter) and the initially-focused button, so
    // neither the keyboard nor the focus highlight lands on the destructive
    // Uninstall action.
    cancelBtn->setDefault(true);
    cancelBtn->setFocus();
    outer->addWidget(footer);

    int choice = 0;   // 0 = cancel, 1 = uninstall, 2 = reinstall
    connect(uninstallBtn, &QPushButton::clicked, &dlg, [&]{ choice = 1; dlg.accept(); });
    connect(reinstallBtn, &QPushButton::clicked, &dlg, [&]{ choice = 2; dlg.accept(); });
    connect(cancelBtn,    &QPushButton::clicked, &dlg, &QDialog::reject);

    const QString sound = QStringLiteral("/usr/share/sounds/Windows 7/og/Windows Exclamation.wav");
    if (QFileInfo::exists(sound)) {
        if (!m_dialogSound) {
            m_dialogSound = new QSoundEffect(this);
            m_dialogSound->setSource(QUrl::fromLocalFile(sound));
            m_dialogSound->setVolume(1.0f);
        }
        m_dialogSound->play();
    }
    dlg.exec();
    if      (choice == 1) runPackageAction(pkg, display, /*reinstall=*/false);
    else if (choice == 2) runPackageAction(pkg, display, /*reinstall=*/true);
}

void ProgramsFeaturesPage::runPackageAction(const QString &pkg, const QString &display,
                                            bool reinstall)
{
    m_service->startAction(pkg, display, reinstall);
}
