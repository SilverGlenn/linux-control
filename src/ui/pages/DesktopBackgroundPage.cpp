#include "DesktopBackgroundPage.h"
#include "settings/WallpaperSettings.h"
#include "platform/kde/WallpaperBackend.h"
#include "Win7Ui.h"
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

QList<SidebarLink> DesktopBackgroundPage::sidebarLinks()
{
    return {Nav::to(tr("Personalization"), PageId::Personalization),
            Nav::to(tr("Icons, pointers and application style"), PageId::ThemeComponents)};
}
QList<SidebarLink> DesktopBackgroundPage::sidebarSeeAlso()
{
    return {Nav::to(tr("Screen Resolution"), PageId::Display), Nav::to(tr("Fonts"), PageId::Fonts)};
}
DesktopBackgroundPage::DesktopBackgroundPage(QScrollArea *sidebar, QWidget *parent)
    : DesktopBackgroundPage(sidebar, WallpaperSettings::instance(), parent) {}
DesktopBackgroundPage::DesktopBackgroundPage(QScrollArea *sidebar, WallpaperSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 700);
    layout->addWidget(Win7::pageTitle(tr("Choose your desktop background")));
    auto *intro = Win7::label(tr("Choose a picture for the selected Plasma desktop. Your other screens and activities keep their own backgrounds."), 9, "#222222");
    intro->setWordWrap(true); layout->addWidget(intro); layout->addSpacing(16);
    auto *desktopRow = new QHBoxLayout;
    auto *desktopLabel = Win7::label(tr("&Desktop:"), 9, "#222222");
    m_desktop = new QComboBox; m_desktop->setObjectName("wallpaperDesktop");
    desktopLabel->setBuddy(m_desktop); m_desktop->setAccessibleName(tr("Plasma desktop"));
    desktopRow->addWidget(desktopLabel); desktopRow->addWidget(m_desktop, 1); layout->addLayout(desktopRow);
    layout->addSpacing(12);
    m_preview = new QLabel(tr("No picture preview")); m_preview->setObjectName("wallpaperPreview");
    m_preview->setAlignment(Qt::AlignCenter); m_preview->setMinimumSize(320, 200); m_preview->setMaximumHeight(250);
    m_preview->setStyleSheet("background: #E9EDF2; color: #444444; border: 1px solid #A9B5C4;");
    m_preview->setAccessibleName(tr("Selected desktop background preview")); layout->addWidget(m_preview);
    layout->addSpacing(12);
    auto *fileRow = new QHBoxLayout;
    m_path = Win7::label(QString(), 9, "#333333"); m_path->setObjectName("wallpaperPath");
    m_path->setWordWrap(true); m_path->setTextFormat(Qt::PlainText);
    m_path->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_browse = new QPushButton(tr("&Browse…")); m_browse->setObjectName("browseWallpaper");
    fileRow->addWidget(m_path, 1); fileRow->addWidget(m_browse); layout->addLayout(fileRow);
    layout->addSpacing(16);
    auto *positionRow = new QHBoxLayout;
    auto *positionLabel = Win7::label(tr("Picture &position:"), 9, "#222222");
    m_position = new QComboBox; m_position->setObjectName("wallpaperPosition"); positionLabel->setBuddy(m_position);
    m_position->setAccessibleName(tr("Picture position"));
    m_position->addItem(tr("Fill"), 2); m_position->addItem(tr("Fit"), 1);
    m_position->addItem(tr("Stretch"), 0); m_position->addItem(tr("Center"), 6);
    positionRow->addWidget(positionLabel); positionRow->addWidget(m_position); positionRow->addStretch();
    layout->addLayout(positionRow); layout->addSpacing(16);
    m_status = Win7::label(QString(), 9, "#333333"); m_status->setObjectName("wallpaperStatus");
    m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText); layout->addWidget(m_status);
    auto *buttons = new QHBoxLayout; buttons->addStretch();
    m_apply = new QPushButton(tr("&Apply")); m_apply->setObjectName("applyWallpaper");
    m_revert = new QPushButton(tr("&Revert")); m_revert->setObjectName("revertWallpaper");
    buttons->addWidget(m_apply); buttons->addWidget(m_revert); layout->addLayout(buttons); layout->addStretch();
    Win7::styleSettingsControls(this);
    connect(m_desktop, &QComboBox::activated, this, [this] { m_session->selectDesktop(m_desktop->currentData().toUInt()); });
    connect(m_position, &QComboBox::activated, this, [this] { m_session->selectFillMode(m_position->currentData().toInt()); });
    connect(m_browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Choose a desktop background"),
            QUrl(m_session->stagedImage()).toLocalFile(), tr("Pictures (*.png *.jpg *.jpeg *.webp *.bmp *.gif);;All files (*)"));
        if (!path.isEmpty()) m_session->selectPicture(path);
    });
    connect(m_apply, &QPushButton::clicked, session, &WallpaperSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &WallpaperSettings::revert);
    connect(session, &SettingsSession::stateChanged, this, &DesktopBackgroundPage::refresh); refresh();
}
SettingsSession *DesktopBackgroundPage::settingsSession() const { return m_session; }

void DesktopBackgroundPage::refresh()
{
    QSignalBlocker desktopBlock(m_desktop), positionBlock(m_position);
    m_desktop->clear();
    for (const auto &desktop : m_session->state().desktops)
        m_desktop->addItem(desktop.screen >= 0 ? tr("Screen %1 — desktop %2").arg(desktop.screen + 1).arg(desktop.id)
                           : tr("Inactive desktop %1").arg(desktop.id), desktop.id);
    m_desktop->setCurrentIndex(m_desktop->findData(m_session->selectedDesktop()));
    const bool editable = m_session->canApply() && !m_session->isBusy() && !m_session->hasConflict();
    m_desktop->setEnabled(m_session->isReady() && !m_session->isBusy() && !m_session->isDirty());
    m_browse->setEnabled(editable); m_position->setEnabled(editable);
    if (m_position->count() > 4) m_position->removeItem(4);
    int index = m_position->findData(m_session->stagedFillMode());
    if (index < 0) { m_position->addItem(tr("Current KDE position (%1)").arg(m_session->stagedFillMode()), m_session->stagedFillMode()); index = 4; }
    m_position->setCurrentIndex(index);
    const QString source = m_session->stagedImage();
    m_path->setText(source.isEmpty() ? tr("Choose a picture with Browse.") : QUrl(source).toDisplayString(QUrl::PreferLocalFile));
    if (source != m_previewSource) {
        m_previewSource = source; QString error;
        m_preview->clear();
        if (validateWallpaperPicture(source, error)) {
            QImageReader reader(QUrl(source).toLocalFile()); reader.setAutoTransform(true);
            reader.setScaledSize(reader.size().scaled(460, 230, Qt::KeepAspectRatio));
            m_preview->setPixmap(QPixmap::fromImage(reader.read()));
        } else m_preview->setText(tr("Preview unavailable\nChoose a local picture with Browse."));
    }
    QString status = m_session->isBusy() ? tr("Checking Plasma desktop backgrounds…")
        : m_session->isDirty() ? tr("Selected picture or position has not been applied.") : tr("Current background read from Plasma.");
    if (const auto *desktop = m_session->desktop()) {
        if (!desktop->reason.isEmpty()) status = desktop->reason;
        else if (desktop->plugin != QStringLiteral("org.kde.image"))
            status += tr("\nThis desktop currently uses %1. Applying a picture replaces that wallpaper on this desktop.").arg(desktop->plugin);
    } else if (!m_session->isBusy()) status = tr("No Plasma desktop is available.");
    if (m_session->hasConflict()) status += tr("\nRevert to reload the current desktop before applying.");
    if (!m_session->errorText().isEmpty()) status += '\n' + m_session->errorText();
    else if (!m_session->resultText().isEmpty()) status += '\n' + m_session->resultText();
    m_status->setText(status);
    m_status->setStyleSheet(m_session->errorText().isEmpty() ? "color: #333333; background: transparent;" : "color: #A02C20; background: transparent;");
    m_apply->setEnabled(editable && m_session->isDirty() && WallpaperSettings::supportedFillMode(m_session->stagedFillMode()));
    m_revert->setEnabled(!m_session->isBusy());
}
