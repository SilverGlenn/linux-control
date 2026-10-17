#include "FontsPage.h"
#include "settings/UserFontService.h"
#include "LinkLabel.h"
#include "Win7Ui.h"

#include <QScrollArea>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QFont>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QFile>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>
#include <QTextEdit>
#include <QKeyEvent>
#include <QTimer>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QSet>
#include <algorithm>

namespace {

// A single Windows 7 font tile: a stacked "document" icon carrying an "Abg"
// preview drawn in the family's own typeface, with the family name centred
// underneath. Selection/hover paint a rounded highlight behind the whole tile.
//
// Kept a plain QWidget (no Q_OBJECT/signals): FontsPage drives selection and
// activation through an installed event filter, exactly like the old rows, so
// no moc is needed for this in-.cpp helper.
class FontTile : public Win7::FocusWidget {
public:
    explicit FontTile(const QString &family, QWidget *parent = nullptr)
        : Win7::FocusWidget(parent), m_family(family)
    {
        setAccessibleName(family);
        setAccessibleDescription(QStringLiteral("Press Enter to preview this font."));
        setFixedSize(88, 116);
        setCursor(Qt::PointingHandCursor);

        m_previewFont = QFont(family);
        m_previewFont.setPointSize(13);

        // Latin faces preview as "Abg" (as in Windows 7); a face with no Latin
        // 'A' has nothing to say with that sample, so fall back to its name.
        QFontMetrics fm(m_previewFont);
        m_sample = fm.inFont(QChar(u'A')) ? QStringLiteral("Abg") : m_family;
    }

    void setSelected(bool sel)
    {
        if (m_selected != sel) { m_selected = sel; update(); }
    }

protected:
    void enterEvent(QEnterEvent *e) override { m_hover = true; update(); QWidget::enterEvent(e); }
    void leaveEvent(QEvent *e) override      { m_hover = false; update(); QWidget::leaveEvent(e); }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        // Whole-tile highlight for selection/hover.
        if (m_selected || m_hover) {
            const QColor fill  = m_selected ? QColor(0xCC, 0xE8, 0xFF) : QColor(0xE9, 0xF4, 0xFE);
            const QColor edge  = m_selected ? QColor(0x99, 0xC7, 0xF0) : QColor(0xB8, 0xDD, 0xF6);
            const QRectF hi = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
            p.setPen(edge);
            p.setBrush(fill);
            p.drawRoundedRect(hi, 3, 3);
        }

        // Document icon: a front sheet with a folded top-right corner, plus a
        // second sheet peeking behind it for the "stack of faces" look.
        const qreal iw = 44, ih = 54, fold = 11;
        const qreal ix = (width() - iw - 3) / 2.0, iy = 9;
        const QRectF front(ix, iy, iw, ih);
        const QRectF back = front.translated(3, 3);

        p.setPen(QColor(0xC4, 0xC8, 0xD0));
        p.setBrush(Qt::white);
        p.drawPath(pagePath(back, fold));

        const QPainterPath frontPath = pagePath(front, fold);
        p.setPen(QColor(0xA9, 0xAE, 0xB8));
        p.setBrush(Qt::white);
        p.drawPath(frontPath);

        // The dog-eared corner.
        QPainterPath ear;
        ear.moveTo(front.right() - fold, front.top());
        ear.lineTo(front.right() - fold, front.top() + fold);
        ear.lineTo(front.right(),        front.top() + fold);
        ear.closeSubpath();
        p.setPen(QColor(0xC4, 0xC8, 0xD0));
        p.setBrush(QColor(0xD9, 0xDD, 0xE5));
        p.drawPath(ear);

        // The preview sample, clipped to the sheet so wide glyphs don't spill.
        p.save();
        p.setClipPath(frontPath);
        p.setFont(m_previewFont);
        p.setPen(Qt::black);
        p.drawText(front.adjusted(2, fold, -2, -2), Qt::AlignCenter, m_sample);
        p.restore();

        // The family name, centred and word-wrapped to (at most) two lines.
        QFont nameFont = font();
        nameFont.setPointSize(8);
        p.setFont(nameFont);
        p.setPen(QColor(0x1E, 0x1E, 0x1E));
        p.drawText(QRect(2, int(iy + ih + 6), width() - 4, 38),
                   Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, m_family);
        Win7::paintFocus(this, p);
    }

private:
    // A page outline with a folded top-right corner.
    static QPainterPath pagePath(const QRectF &r, qreal fold)
    {
        QPainterPath path;
        path.moveTo(r.left(),         r.top());
        path.lineTo(r.right() - fold, r.top());
        path.lineTo(r.right(),        r.top() + fold);
        path.lineTo(r.right(),        r.bottom());
        path.lineTo(r.left(),         r.bottom());
        path.closeSubpath();
        return path;
    }

    QString m_family;
    QString m_sample;
    QFont   m_previewFont;
    bool    m_selected = false;
    bool    m_hover    = false;
};

} // namespace

QList<SidebarLink> FontsPage::sidebarLinks()
{
    return {
        Nav::to("Font settings", PageId::FontSettings),
        Nav::to("Adjust ClearType text", PageId::FontSettings),
        Nav::plain("Find a character"),
    };
}
QList<SidebarLink> FontsPage::sidebarSeeAlso()
{
    return { Nav::to("Personalization", PageId::Personalization) };
}
FontsPage::FontsPage(QScrollArea *sidebar, QWidget *parent)
    : FontsPage(sidebar, UserFontService::instance(), parent) {}
FontsPage::FontsPage(QScrollArea *sidebar, UserFontService *service, QWidget *parent)
    : QWidget(parent), m_service(service)
{
    setObjectName("fontsPage");
    auto *content = Win7::pageScaffold(this, sidebar, 0, 700);
    content->setContentsMargins(0, 18, 0, 0);
    auto *heading = new QVBoxLayout;
    heading->setContentsMargins(28, 0, 28, 0);
    heading->addWidget(Win7::pageTitle("Preview the fonts installed on your computer"));
    auto *explanation = Win7::label("Select a font to preview it. Install and remove fonts for your account.", 9, "#555555");
    explanation->setWordWrap(true);
    heading->addWidget(explanation);
    content->addLayout(heading);
    content->addSpacing(10);

    QHBoxLayout *commands = nullptr;
    content->addWidget(Win7::commandBar(&commands));
    const auto command = [commands](const QString &text, const char *name) {
        auto *button = new QPushButton(text);
        button->setObjectName(name);
        button->setFlat(true);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { color: #222222; background: transparent; border: 1px solid transparent;"
            " border-radius: 2px; padding: 3px 10px; }"
            "QPushButton:hover { background: #E8F4FC; border-color: #9EC8E4; }"
            "QPushButton:pressed { background: #C4E5F6; }"
            "QPushButton:focus { border: 1px dotted #245B96; }"
            "QPushButton:disabled { color: #777777; }"));
        commands->addWidget(button);
        return button;
    };
    m_refresh = command("Refresh", "fontRefresh");
    m_preview = command("Preview", "fontPreview");
    m_install = command("Install…", "fontInstall");
    m_remove = command("Delete", "fontRemove");
    commands->addStretch();
    m_search = new QLineEdit;
    m_search->setObjectName("fontSearch");
    m_search->setAccessibleName("Search fonts");
    m_search->setPlaceholderText("Search fonts");
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(200);
    m_search->setStyleSheet(QStringLiteral(
        "QLineEdit { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; padding: 3px 6px; }"
        "QLineEdit:focus { border-color: #3C7FB1; }"));
    commands->addWidget(m_search);

    m_scroll = new QScrollArea;
    m_scroll->setObjectName("fontInventory");
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setMinimumHeight(300);
    auto *gridHost = new QWidget;
    gridHost->setStyleSheet("background: white;");
    m_grid = new QGridLayout(gridHost);
    m_grid->setContentsMargins(8, 8, 8, 8);
    m_grid->setHorizontalSpacing(4);
    m_grid->setVerticalSpacing(8);
    m_scroll->setWidget(gridHost);
    content->addWidget(m_scroll, 1);
    m_details = Win7::label("", 9, "#555555");
    m_details->setObjectName("fontSelectionDetails");
    m_details->setWordWrap(true);
    m_details->setContentsMargins(16, 5, 16, 5);
    content->addWidget(m_details);
    QHBoxLayout *statusLayout = nullptr;
    auto *statusPanel = Win7::statusPanel(40, &statusLayout);
    statusPanel->setMaximumHeight(QWIDGETSIZE_MAX);
    content->addWidget(statusPanel);
    m_status = Win7::label("", 9, "#333333");
    m_status->setObjectName("fontStatus");
    m_status->setWordWrap(true);
    statusLayout->addWidget(m_status);
    content->addSpacing(20);

    connect(m_refresh, &QPushButton::clicked, service, &UserFontService::refresh);
    connect(m_preview, &QPushButton::clicked, this, &FontsPage::openSelected);
    connect(m_remove, &QPushButton::clicked, this, &FontsPage::removeSelected);
    connect(m_install, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "Install a font for your account", {},
                                                         "Font files (*.ttf *.otf *.ttc);;All files (*)");
        if (!path.isEmpty()) m_service->installFont(path);
    });
    connect(m_search, &QLineEdit::textChanged, this, &FontsPage::rebuild);
    connect(service, &UserFontService::changed, this, [this] { rebuild(); });
    rebuild();
    if (!service->busy() && !service->inventoryValid()) QTimer::singleShot(0, service, &UserFontService::refresh);
}
void FontsPage::rebuild()
{
    const auto families = m_service->families();
    QString selected;
    if (m_selected >= 0 && m_selected < m_rows.size()) selected = m_rows[m_selected]->accessibleName();
    m_rowToIndex.clear(); m_visible.clear();
    while (auto *item = m_grid->takeAt(0)) { m_rows.removeAll(item->widget()); delete item->widget(); delete item; }
    qDeleteAll(m_rows); m_rows.clear();
    for (int row = 0; row < m_grid->rowCount(); ++row) m_grid->setRowStretch(row, 0);
    m_selected = -1;
    for (int i = 0; i < families.size(); ++i) {
        auto *tile = new FontTile(families[i].name, m_scroll->widget());
        tile->setObjectName("fontTile");
        tile->setAccessibleDescription("Press Enter or Space to preview this font. Use arrow keys to select fonts.");
        tile->installEventFilter(this);
        m_rowToIndex.insert(tile, i); m_rows.append(tile);
        const bool visible = families[i].name.contains(m_search->text(), Qt::CaseInsensitive);
        tile->setVisible(visible);
        // All tiles have the grid host as parent, including filtered tiles.
        tile->setParent(m_scroll->widget());
        if (visible) {
            const int position = m_visible.size();
            m_grid->addWidget(tile, position / 7, position % 7, Qt::AlignLeft | Qt::AlignTop);
            m_visible.append(i);
            if (families[i].name == selected) m_selected = i;
        }
    }
    m_grid->setColumnStretch(7, 1);
    m_grid->setRowStretch(m_visible.size() / 7 + 1, 1);
    if (m_visible.isEmpty()) {
        auto *empty = Win7::label(m_service->busy() ? "Reading installed fonts…" :
                                  (families.isEmpty() ? "No font inventory is available. Use Refresh to try again." : "No fonts match your search."), 9, "#555555");
        empty->setWordWrap(true);
        m_grid->addWidget(empty, 0, 0, 1, 7);
    }
    if (m_selected >= 0) static_cast<FontTile *>(m_rows[m_selected])->setSelected(true);
    updateState();
}
void FontsPage::updateState()
{
    const auto families = m_service->families();
    const bool selected = m_selected >= 0 && m_selected < families.size();
    const bool busy = m_service->busy();
    m_refresh->setEnabled(!busy);
    m_install->setEnabled(!busy);
    m_preview->setEnabled(selected);
    m_remove->setEnabled(selected && !busy && m_service->canRemove(families[m_selected].file));
    m_status->setText(m_service->result() + QStringLiteral("  ·  %1 of %2 families shown").arg(m_visible.size()).arg(families.size()));
    if (selected) {
        const bool managed = m_service->canRemove(families[m_selected].file);
        m_details->setText(families[m_selected].name + (managed ? " — Installed by this Control Panel for your account." :
                       " — System or unmanaged font. Removal is unavailable; only fonts installed here can be deleted."));
    } else m_details->setText("Install adds a validated font to your personal fonts folder. System fonts are managed by your distribution.");
}
void FontsPage::selectRow(int index)
{
    if (!m_visible.contains(index)) return;
    m_selected = index;
    for (int i = 0; i < m_rows.size(); ++i) static_cast<FontTile *>(m_rows[i])->setSelected(i == index);
    m_scroll->ensureWidgetVisible(m_rows[index]);
    updateState();
}
void FontsPage::openSelected()
{
    const auto families = m_service->families();
    if (m_selected < 0 || m_selected >= families.size()) return;
    const auto family = families[m_selected];
    auto *dialog = new QDialog(this);
    dialog->setObjectName("fontPreviewDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(family.name + " — Font preview");
    dialog->resize(620, 390);
    Win7::styleSettingsDialog(dialog);
    auto *layout = new QVBoxLayout(dialog);
    // Register the exact face in memory while the preview is open. This also
    // previews a newly installed font before Qt refreshes its font database.
    QString previewFamily = family.name;
    int fontId = -1;
    const int fd = ::open(QFile::encodeName(family.file).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st {};
    if (fd >= 0 && ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 32 * 1024 * 1024) {
        QFile source;
        if (source.open(fd, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) {
            const QByteArray bytes = source.read(32 * 1024 * 1024 + 1);
            if (bytes.size() == st.st_size) fontId = QFontDatabase::addApplicationFontFromData(bytes);
        }
    }
    if (fd >= 0) ::close(fd);
    if (fontId >= 0) {
        const QStringList names = QFontDatabase::applicationFontFamilies(fontId);
        if (!names.isEmpty()) previewFamily = names.contains(family.name) ? family.name : names.first();
        connect(dialog, &QObject::destroyed, [fontId] { QFontDatabase::removeApplicationFont(fontId); });
    } else {
        auto *fallback = new QLabel("The original face is unavailable. This sample uses the installed family or a fallback font.");
        fallback->setWordWrap(true);
        layout->addWidget(fallback);
    }
    auto *title = new QLabel(family.name);
    QFont bold = title->font(); bold.setBold(true); title->setFont(bold);
    layout->addWidget(title);
    auto *file = new QLabel(family.file); file->setTextInteractionFlags(Qt::TextSelectableByMouse);
    file->setWordWrap(true); layout->addWidget(file);
    auto *controls = new QHBoxLayout;
    controls->addWidget(new QLabel("Size:"));
    auto *size = new QSpinBox; size->setObjectName("fontPreviewSize");
    size->setRange(6, 144); size->setValue(24); size->setSuffix(" pt");
    controls->addWidget(size); controls->addStretch(); layout->addLayout(controls);
    auto *sample = new QTextEdit;
    sample->setObjectName("fontPreviewSample");
    sample->setAccessibleName("Editable font preview sample");
    sample->setPlainText("The quick brown fox jumps over the lazy dog.\nABCDEFGHIJKLMNOPQRSTUVWXYZ\nabcdefghijklmnopqrstuvwxyz\n0123456789 !@#$%&()\n\nType your own text here.");
    const auto updateFont = [sample, previewFamily](int points) { sample->setFont(QFont(previewFamily, points)); };
    updateFont(24);
    connect(size, &QSpinBox::valueChanged, dialog, updateFont);
    layout->addWidget(sample, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->open();
}
void FontsPage::removeSelected()
{
    const auto families = m_service->families();
    if (m_selected < 0 || m_selected >= families.size()) return;
    const auto family = families[m_selected];
    if (!m_service->canRemove(family.file)) return;
    auto *confirm = new QMessageBox(QMessageBox::Question, "Delete current-user font",
        "Delete the font file for “" + family.name + "” installed by this Control Panel?\n\nOther font files and system fonts are unaffected.",
        QMessageBox::Yes | QMessageBox::No, this);
    confirm->setDefaultButton(QMessageBox::No);
    confirm->setAttribute(Qt::WA_DeleteOnClose);
    connect(confirm, &QMessageBox::finished, m_service, [service = m_service, path = family.file](int result) {
        if (result == QMessageBox::Yes) service->removeFont(path);
    });
    confirm->open();
}
bool FontsPage::eventFilter(QObject *watched, QEvent *event)
{
    const auto found = m_rowToIndex.constFind(watched);
    if (found == m_rowToIndex.constEnd()) return QWidget::eventFilter(watched, event);
    const int current = found.value();
    if (event->type() == QEvent::FocusIn) selectRow(current);
    if (event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        int position = m_visible.indexOf(current), next = position;
        switch (key->key()) {
        case Qt::Key_Left: --next; break;
        case Qt::Key_Right: ++next; break;
        case Qt::Key_Up: next -= 7; break;
        case Qt::Key_Down: next += 7; break;
        case Qt::Key_Home: next = 0; break;
        case Qt::Key_End: next = m_visible.size() - 1; break;
        case Qt::Key_Space:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (!key->isAutoRepeat()) { selectRow(current); openSelected(); }
            return true;
        default: return QWidget::eventFilter(watched, event);
        }
        if (!m_visible.isEmpty()) {
            const int index = m_visible[qBound(0, next, int(m_visible.size()) - 1)];
            m_rows[index]->setFocus(Qt::OtherFocusReason); selectRow(index);
        }
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        static_cast<QWidget *>(watched)->setFocus(Qt::MouseFocusReason); selectRow(current); return true;
    }
    if (event->type() == QEvent::MouseButtonDblClick && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        selectRow(current); openSelected(); return true;
    }
    return QWidget::eventFilter(watched, event);
}
