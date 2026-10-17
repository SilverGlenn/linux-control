#include "KeyboardPreview.h"
#include "settings/KeyboardSettings.h"
#include "ui/Win7Ui.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <xkbcommon/xkbcommon.h>
#include <algorithm>
#include <atomic>
#include <memory>

namespace {
QString symbolLabel(xkb_keysym_t symbol)
{
    const uint code = xkb_keysym_to_utf32(symbol);
    if (code && QChar::isPrint(code)) {
        if (code == ' ') return QObject::tr("Space");
        const char32_t c = code;
        const QString character = QString::fromUcs4(&c, 1);
        const auto category = QChar::category(code);
        return category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing
            ? QStringLiteral("◌") + character : character;
    }
    char name[128] = {};
    if (xkb_keysym_get_name(symbol, name, sizeof(name)) <= 0) return QObject::tr("No symbol");
    const QString text = QString::fromLatin1(name);
    static const QMap<QString, QString> shortNames{
        {"BackSpace", "Backspace"}, {"ISO_Level3_Shift", "AltGr"}, {"ISO_Level5_Shift", "Level 5"},
        {"Shift_L", "Shift"}, {"Shift_R", "Shift"}, {"Control_L", "Ctrl"}, {"Control_R", "Ctrl"},
        {"Alt_L", "Alt"}, {"Alt_R", "Alt"}, {"Super_L", "Win"}, {"Super_R", "Win"},
        {"Caps_Lock", "Caps Lock"}, {"Num_Lock", "Num Lock"}, {"Scroll_Lock", "Scroll Lock"},
        {"Page_Up", "Page Up"}, {"Page_Down", "Page Down"}, {"Return", "Enter"}};
    return shortNames.value(text, text);
}

// Geometry coordinates and key order are derived from xkeyboard-config geometry/pc
// pc101/pc102/pc104/pc105 (470 x 180 mm), selected by rules/evdev's $pcmodels.
// Copyright 1996, 1998 The Open Group.
// Permission to use, copy, modify, distribute, and sell this software and its
// documentation for any purpose is hereby granted without fee, provided that
// the above copyright notice appear in all copies and that both that copyright
// notice and this permission notice appear in supporting documentation.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// Except as contained in this notice, the name of The Open Group shall not be
// used in advertising or otherwise to promote the sale, use or other dealings
// in this Software without prior written authorization from The Open Group.
QList<KeyboardPreview::Shape> pcGeometry(const QString &model)
{
    if (model != "pc101" && model != "pc102" && model != "pc104" && model != "pc105") return {};
    QList<KeyboardPreview::Shape> shapes;
    const bool iso = model == "pc102" || model == "pc105";
    const bool windows = model == "pc104" || model == "pc105";
    auto key = [&](const QString &name, double x, double y, double w = 18, double h = 18) {
        shapes.append({name, QPolygonF(QRectF(x, y, w, h))});
    };
    auto row = [&](const QStringList &names, double x, double y) {
        for (const auto &name : names) { key(name, x, y); x += 19; }
    };
    key("ESC", 20, 23);
    double x = 58;
    for (int i = 1; i <= 12; ++i) {
        if (i == 5 || i == 9) x += 10;
        key(QStringLiteral("FK%1").arg(i, 2, 10, QLatin1Char('0')), x, 23); x += 19;
    }
    row({"PRSC", "SCLK", "PAUS"}, x + 7, 23);
    key("TLDE", 20, 62);
    for (int i = 1; i <= 12; ++i) key(QStringLiteral("AE%1").arg(i, 2, 10, QLatin1Char('0')), 20 + i * 19, 62);
    key("BKSP", 267, 62, 38);
    key("TAB", 20, 81, 28);
    for (int i = 1; i <= 12; ++i) key(QStringLiteral("AD%1").arg(i, 2, 10, QLatin1Char('0')), 49 + (i - 1) * 19, 81);
    if (iso) {
        QPolygonF enter{{0,0},{28,0},{28,37},{5,37},{5,18},{0,18}};
        enter.translate(277, 81); shapes.append({"RTRN", enter});
    } else key("BKSL", 277, 81, 28);
    key("CAPS", 20, 100, 33);
    for (int i = 1; i <= 11; ++i) key(QStringLiteral("AC%1").arg(i, 2, 10, QLatin1Char('0')), 54 + (i - 1) * 19, 100);
    if (iso) key("BKSL", 263, 100);
    else key("RTRN", 263, 100, 42);
    key("LFSH", 20, 119, iso ? 23 : 42);
    if (iso) key("LSGT", 44, 119);
    for (int i = 1; i <= 10; ++i) key(QStringLiteral("AB%1").arg(i, 2, 10, QLatin1Char('0')), 63 + (i - 1) * 19, 119);
    key("RTSH", 253, 119, 52);
    key("LCTL", 20, 138, 28);
    if (windows) {
        key("LWIN", 49, 138, 23); key("LALT", 73, 138, 23); key("SPCE", 97, 138, 107);
        key("RALT", 205, 138, 23); key("RWIN", 229, 138, 23); key("MENU", 253, 138, 23); key("RCTL", 277, 138, 28);
    } else {
        key("LALT", 67, 138, 28); key("SPCE", 96, 138, 133); key("RALT", 230, 138, 28); key("RCTL", 277, 138, 28);
    }
    row({"INS", "HOME", "PGUP"}, 313, 62); row({"DELE", "END", "PGDN"}, 313, 81);
    key("UP", 332, 119); row({"LEFT", "DOWN", "RGHT"}, 313, 138);
    row({"NMLK", "KPDV", "KPMU", "KPSU"}, 377, 62);
    row({"KP7", "KP8", "KP9"}, 377, 81); key("KPAD", 434, 81, 18, 37);
    row({"KP4", "KP5", "KP6"}, 377, 100);
    row({"KP1", "KP2", "KP3"}, 377, 119); key("KPEN", 434, 119, 18, 37);
    key("KP0", 377, 138, 37); key("KPDL", 415, 138);
    return shapes;
}

class Diagram final : public QWidget {
public:
    using QWidget::QWidget;
    KeyboardPreview::Result result;
    int group = 0, level = 0;
    QString selected;
    std::function<void(const QString &)> selectedKey;
    QSize sizeHint() const override { return {1000, 385}; }
    QTransform transform() const {
        const double scale = std::min((width() - 16) / 470.0, (height() - 16) / 180.0);
        QTransform t; t.translate((width() - 470 * scale) / 2, (height() - 180 * scale) / 2); t.scale(scale, scale); return t;
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), QColor("#F0F0F0")); p.setTransform(transform());
        p.setPen(QPen(QColor("#ABADB3"), .5)); p.setBrush(QColor("#E5E9EE")); p.drawRoundedRect(QRectF(0, 0, 470, 180), 3, 3);
        for (const auto &shape : result.geometry) {
            const QRectF bounds = shape.outline.boundingRect();
            QLinearGradient g(bounds.topLeft(), bounds.bottomLeft());
            g.setColorAt(0, selected == shape.name ? QColor("#D9F0FF") : QColor("#FFFFFF"));
            g.setColorAt(1, selected == shape.name ? QColor("#A9D7F3") : QColor("#E6E8EB"));
            p.setBrush(g); p.setPen(QPen(QColor(selected == shape.name ? "#3C7FB1" : "#8E8F8F"), .5)); p.drawPolygon(shape.outline);
            const auto *k = result.key(shape.name);
            QString label;
            if (k && group < k->groups.size()) {
                const auto &levels = k->groups[group].levels;
                // An absent level is not silently replaced with a different level.
                label = level < levels.size() ? levels[level].join(QStringLiteral(" + ")) : QString();
            }
            QFont f = font(); f.setPixelSize(label.size() > 5 ? 4 : 6); p.setFont(f); p.setPen(QColor("#000000"));
            p.drawText(bounds.adjusted(1, 1, -1, -1), Qt::AlignCenter | Qt::TextWordWrap, label);
        }
    }
    void mousePressEvent(QMouseEvent *event) override {
        const QPointF point = transform().inverted().map(event->position());
        for (const auto &shape : result.geometry) if (shape.outline.containsPoint(point, Qt::OddEvenFill)) {
            selected = shape.name; update(); if (selectedKey) selectedKey(selected); break;
        }
    }
};
std::atomic_bool previewRunning = false;
QThreadPool &previewPool() { static QThreadPool pool; pool.setMaxThreadCount(1); return pool; }
}

const KeyboardPreview::Key *KeyboardPreview::Result::key(const QString &name) const
{
    for (const auto &entry : keys) if (entry.name == name) return &entry;
    return nullptr;
}

KeyboardPreview::Result KeyboardPreview::compile(const Request &request)
{
    Result result; result.request = request;
    const QString root = QStringLiteral("/usr/share/xkeyboard-config-2");
    const auto registry = KeyboardTypes::readRegistry(root + QStringLiteral("/rules/evdev.xml"), root + QStringLiteral("/rules/evdev.extras.xml"));
    if (!registry.valid) { result.error = QObject::tr("The installed XKB registry could not be read: %1").arg(registry.error); return result; }
    const auto *layout = registry.layout(request.layout);
    const auto known = [](const QList<KeyboardChoice> &choices, const QString &id) {
        return std::any_of(choices.cbegin(), choices.cend(), [&](const auto &c) { return c.id == id; });
    };
    if (!known(registry.models, request.model)) result.error = QObject::tr("Keyboard model '%1' is unavailable in the installed XKB registry.").arg(request.model);
    else if (!layout) result.error = QObject::tr("Layout '%1' is unavailable in the installed XKB registry.").arg(request.layout);
    else if (!request.variant.isEmpty() && !known(layout->variants, request.variant)) result.error = QObject::tr("Variant '%1' is unavailable for layout '%2'.").arg(request.variant, request.layout);
    QSet<QString> options;
    for (const auto &group : registry.optionGroups) for (const auto &option : group.options) options.insert(option.id);
    if (request.options.size() > 256) result.error = QObject::tr("There are too many keyboard options to preview.");
    for (const auto &option : request.options) if (!options.contains(option)) { result.error = QObject::tr("Option '%1' is unavailable in the installed XKB registry.").arg(option); break; }
    if (!result.error.isEmpty()) return result;
    const auto flags = xkb_context_flags(XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
    std::unique_ptr<xkb_context, decltype(&xkb_context_unref)> context(xkb_context_new(flags), xkb_context_unref);
    if (!context || !xkb_context_include_path_append(context.get(), root.toUtf8().constData())) {
        result.error = QObject::tr("The installed XKB data could not be opened."); return result;
    }
    xkb_context_set_log_level(context.get(), XKB_LOG_LEVEL_CRITICAL);
    const QByteArray model = request.model.toUtf8(), language = request.layout.toUtf8(), variant = request.variant.toUtf8(), option = request.options.join(QLatin1Char(',')).toUtf8();
    const xkb_rule_names names{"evdev", model.constData(), language.constData(), variant.constData(), option.constData()};
    std::unique_ptr<xkb_keymap, decltype(&xkb_keymap_unref)> map(xkb_keymap_new_from_names(context.get(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS), xkb_keymap_unref);
    if (!map) { result.error = QObject::tr("The installed XKB data could not compile this model, layout, variant and option combination."); return result; }
    const auto groupCount = xkb_keymap_num_layouts(map.get());
    if (groupCount > 32 || xkb_keymap_max_keycode(map.get()) > 65535) { result.error = QObject::tr("This keymap exceeds the preview's supported size."); return result; }
    for (xkb_layout_index_t group = 0; group < groupCount; ++group) result.groups << QString::fromUtf8(xkb_keymap_layout_get_name(map.get(), group));
    for (xkb_keycode_t code = xkb_keymap_min_keycode(map.get()); code <= xkb_keymap_max_keycode(map.get()); ++code) {
        const char *name = xkb_keymap_key_get_name(map.get(), code); if (!name) continue;
        Key key; key.name = QString::fromLatin1(name); key.code = code;
        for (xkb_layout_index_t group = 0; group < groupCount; ++group) {
            Group keyGroup;
            const auto levels = xkb_keymap_num_levels_for_key(map.get(), code, group);
            if (levels > 32) { result.keys.clear(); result.error = QObject::tr("This keymap has more than 32 levels on a key."); return result; }
            for (xkb_level_index_t level = 0; level < levels; ++level) {
                const xkb_keysym_t *symbols = nullptr;
                const int count = xkb_keymap_key_get_syms_by_level(map.get(), code, group, level, &symbols);
                QStringList labels;
                for (int i = 0; i < count; ++i) labels << symbolLabel(symbols[i]);
                keyGroup.levels << labels;
                xkb_mod_mask_t masks[64];
                const size_t masksCount = xkb_keymap_key_get_mods_for_level(map.get(), code, group, level, masks, 64);
                QStringList alternatives;
                for (size_t i = 0; i < std::min(masksCount, size_t(64)); ++i) {
                    QStringList mods;
                    for (xkb_mod_index_t mod = 0; mod < std::min(xkb_keymap_num_mods(map.get()), xkb_mod_index_t(32)); ++mod)
                        if (masks[i] & (xkb_mod_mask_t(1) << mod)) mods << QString::fromUtf8(xkb_keymap_mod_get_name(map.get(), mod));
                    alternatives << (mods.isEmpty() ? QObject::tr("None") : mods.join(QLatin1Char('+')));
                }
                alternatives.removeDuplicates(); keyGroup.modifiers << alternatives.join(QStringLiteral(" / "));
            }
            key.groups << keyGroup;
        }
        result.keys << key;
    }
    result.geometry = pcGeometry(request.model);
    // Geometry uses aliases such as MENU; xkbcommon resolves those to the
    // actual keycode/name (e.g. COMP). Never label an unresolved alias as a symbol.
    for (auto &shape : result.geometry) {
        const xkb_keycode_t code = xkb_keymap_key_by_name(map.get(), shape.name.toLatin1().constData());
        if (code != XKB_KEYCODE_INVALID) shape.name = QString::fromLatin1(xkb_keymap_key_get_name(map.get(), code));
    }
    if (result.geometry.isEmpty()) result.geometryReason = QObject::tr("Physical geometry is unavailable for model '%1'. The key list below shows its compiled symbols, including virtual and group-switch keys; physical positions are not inferred.").arg(request.model);
    return result;
}

KeyboardPreviewDialog::KeyboardPreviewDialog(const KeyboardPreview::Request &request, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("keyboardPreviewDialog")); setWindowTitle(tr("Keyboard Layout Preview"));
    setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    setAttribute(Qt::WA_DeleteOnClose); resize(1080, 750); Win7::styleSettingsDialog(this);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(16, 16, 16, 16); layout->setSpacing(10);
    auto *title = Win7::pageTitle(tr("%1 — %2%3").arg(request.model, request.layout, request.variant.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(request.variant)));
    title->setTextFormat(Qt::PlainText); layout->addWidget(title);
    auto *hint = Win7::bodyLabel(tr("Preview of your staged selection. Choose a group and level to inspect symbols. Click a drawn key or select a row for its modifier combinations. This preview does not change the active keyboard."));
    hint->setTextFormat(Qt::PlainText); layout->addWidget(hint);
    m_status = Win7::bodyLabel(tr("Preparing keyboard preview…")); m_status->setTextFormat(Qt::PlainText); m_status->setObjectName(QStringLiteral("keyboardPreviewStatus")); layout->addWidget(m_status);
    auto *selectors = new QHBoxLayout;
    auto *groupLabel = Win7::label(tr("&Group:")); m_group = new QComboBox; m_group->setObjectName(QStringLiteral("keyboardPreviewGroup")); m_group->setAccessibleName(tr("Keyboard preview group")); groupLabel->setBuddy(m_group);
    auto *levelLabel = Win7::label(tr("&Level:")); m_level = new QComboBox; m_level->setObjectName(QStringLiteral("keyboardPreviewLevel")); m_level->setAccessibleName(tr("Keyboard preview level")); levelLabel->setBuddy(m_level);
    selectors->addWidget(groupLabel); selectors->addWidget(m_group, 1); selectors->addWidget(levelLabel); selectors->addWidget(m_level, 1); layout->addLayout(selectors);
    m_group->setEnabled(false); m_level->setEnabled(false);
    m_diagram = new Diagram; m_diagram->setObjectName(QStringLiteral("keyboardPreviewDiagram")); m_diagram->setAccessibleName(tr("Physical keyboard diagram")); m_diagram->setAccessibleDescription(tr("Use the compiled key list to inspect every key with the keyboard.")); m_diagram->setMinimumHeight(280); m_diagram->hide(); layout->addWidget(m_diagram, 2);
    m_keys = new QTreeWidget; m_keys->setObjectName(QStringLiteral("keyboardPreviewKeys")); m_keys->setAccessibleName(tr("Compiled keys and all symbol levels"));
    m_keys->setRootIsDecorated(false); m_keys->setMinimumHeight(180); m_keys->setUniformRowHeights(true); m_keys->setSelectionMode(QAbstractItemView::SingleSelection); layout->addWidget(m_keys, 1);
    m_details = Win7::bodyLabel(tr("Select a key to see its modifier combinations.")); m_details->setTextFormat(Qt::PlainText); m_details->setObjectName(QStringLiteral("keyboardPreviewKeyDetails")); layout->addWidget(m_details);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("keyboardPreviewClose")); buttons->button(QDialogButtonBox::Close)->setAccessibleName(tr("Close keyboard preview"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject); layout->addWidget(buttons);
    connect(m_group, &QComboBox::currentIndexChanged, this, &KeyboardPreviewDialog::updateGroup);
    connect(m_level, &QComboBox::currentIndexChanged, this, &KeyboardPreviewDialog::updateLevel);
    connect(m_keys, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        auto *diagram = static_cast<Diagram *>(m_diagram); diagram->selected = item ? item->text(0) : QString(); diagram->update();
        const auto *key = item ? m_result.key(item->text(0)) : nullptr;
        const int group = m_group->currentIndex();
        if (key && group >= 0 && group < key->groups.size()) {
            QStringList details;
            for (int i = 0; i < key->groups[group].modifiers.size(); ++i)
                details << tr("Level %1: %2").arg(i + 1).arg(key->groups[group].modifiers[i].isEmpty() ? tr("No modifier combination") : key->groups[group].modifiers[i]);
            m_details->setText(tr("%1 — %2").arg(key->name, details.join(QStringLiteral("; "))));
        } else m_details->setText(tr("Select a key to see its modifier combinations."));
    });
    static_cast<Diagram *>(m_diagram)->selectedKey = [this](const QString &name) {
        for (int i = 0; i < m_keys->topLevelItemCount(); ++i) if (m_keys->topLevelItem(i)->text(0) == name) {
            m_keys->setCurrentItem(m_keys->topLevelItem(i)); m_keys->scrollToItem(m_keys->topLevelItem(i)); break;
        }
    };
    if (previewRunning.exchange(true)) {
        m_result.request = request; m_result.error = tr("Another keyboard preview is being prepared. Close this window and try again shortly."); QTimer::singleShot(0, this, &KeyboardPreviewDialog::populate); return;
    }
    auto *watcher = new QFutureWatcher<KeyboardPreview::Result>(this);
    auto *deadline = new QTimer(this); deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, this, [this, watcher] {
        disconnect(watcher, nullptr, this, nullptr); m_result.error = tr("Preparing this keyboard preview took too long. Close this window and try again."); populate();
    });
    connect(watcher, &QFutureWatcher<KeyboardPreview::Result>::finished, this, [this, watcher, deadline] {
        deadline->stop(); m_result = watcher->result(); watcher->deleteLater(); populate();
    });
    deadline->start(10000);
    watcher->setFuture(QtConcurrent::run(&previewPool(), [request] {
        struct Reset { ~Reset() { previewRunning = false; } } reset;
        return KeyboardPreview::compile(request);
    }));
}

void KeyboardPreviewDialog::populate()
{
    m_status->setText(!m_result.error.isEmpty() ? m_result.error : !m_result.geometryReason.isEmpty() ? m_result.geometryReason
        : tr("Physical geometry: %1. The list includes additional keys defined by XKB. Symbols and modifier combinations are compiled from the installed XKB data.").arg(m_result.request.model));
    if (m_result.valid()) {
        auto *diagram = static_cast<Diagram *>(m_diagram); diagram->result = m_result; m_diagram->setVisible(!m_result.geometry.isEmpty());
        m_group->addItems(m_result.groups); m_group->setEnabled(m_group->count() > 1); updateGroup();
    }
    emit previewReady();
}
void KeyboardPreviewDialog::updateGroup()
{
    if (!m_result.valid() || m_group->currentIndex() < 0) return;
    const int group = m_group->currentIndex();
    int levels = 0;
    for (const auto &key : m_result.keys) if (group < key.groups.size()) levels = std::max(levels, int(key.groups[group].levels.size()));
    const QSignalBlocker blocker(m_level); m_level->clear();
    QStringList headers{tr("XKB key"), tr("Keycode")};
    for (int level = 0; level < levels; ++level) { const QString label = tr("Level %1").arg(level + 1); headers << label; m_level->addItem(label); }
    m_level->setEnabled(levels > 1); m_keys->clear(); m_keys->setColumnCount(headers.size()); m_keys->setHeaderLabels(headers);
    for (const auto &key : m_result.keys) {
        QStringList columns{key.name, QString::number(key.code)};
        if (group < key.groups.size()) for (const auto &symbols : key.groups[group].levels) columns << symbols.join(QStringLiteral(" + "));
        auto *row = new QTreeWidgetItem(m_keys, columns);
        if (group < key.groups.size()) for (int level = 0; level < key.groups[group].modifiers.size(); ++level)
            row->setToolTip(level + 2, tr("Modifier combinations: %1").arg(key.groups[group].modifiers[level]));
    }
    m_keys->header()->setSectionResizeMode(QHeaderView::ResizeToContents); updateLevel();
}
void KeyboardPreviewDialog::updateLevel()
{
    auto *diagram = static_cast<Diagram *>(m_diagram); diagram->group = std::max(0, m_group->currentIndex()); diagram->level = std::max(0, m_level->currentIndex()); diagram->update();
}
