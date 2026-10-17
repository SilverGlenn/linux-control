#include "CatalogView.h"

#include "ActionCatalog.h"
#include "Branding.h"
#include "IconHelper.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QResizeEvent>
#include <QSet>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

namespace {
enum ItemRole {
    ActionRole = Qt::UserRole,
    DescriptionRole,
    CategoryRole,
    ReasonRole
};

constexpr int TextMargin = 8;

class CatalogDelegate final : public QStyledItemDelegate {
public:
    CatalogDelegate(bool search, QObject *parent)
        : QStyledItemDelegate(parent), m_search(search) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto *list = qobject_cast<const QListWidget *>(parent());
        if (!m_search)
            return list && list->gridSize().isValid() ? list->gridSize() : QSize(280, 54);
        const int width = list ? list->viewport()->width() : 600;
        const int textWidth = qMax(1, width - 32 - TextMargin * 4);
        const QFontMetrics fm(option.font);
        int height = fm.height();
        for (int role : {CategoryRole, DescriptionRole, ReasonRole}) {
            const QString text = index.data(role).toString();
            if (!text.isEmpty())
                height += 3 + fm.boundingRect(QRect(0, 0, textWidth, 10000),
                    Qt::TextWordWrap | Qt::TextWrapAnywhere, text).height();
        }
        return QSize(width, qMax(48, height + TextMargin * 2));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = QIcon();
        background.state &= ~QStyle::State_HasFocus;
        const QWidget *widget = option.widget;
        const QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

        painter->save();
        painter->setClipRect(option.rect);
        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
        const int iconSize = m_search ? 32 : option.decorationSize.width();
        const QRect iconRect(option.rect.left() + TextMargin,
            m_search ? option.rect.top() + TextMargin : option.rect.center().y() - iconSize / 2,
            iconSize, iconSize);
        qvariant_cast<QIcon>(index.data(Qt::DecorationRole)).paint(painter, iconRect,
            Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);

        QRect textRect = option.rect.adjusted(iconSize + TextMargin * 2, TextMargin,
                                              -TextMargin, -TextMargin);
        painter->setFont(option.font);
        painter->setPen(enabled ? QColor(m_search ? "#1E7B1E" : "#1F4E99") : QColor("#777777"));
        if (!m_search) {
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                              Qt::ElideRight, textRect.width()));
        } else {
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignTop,
                option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                              Qt::ElideRight, textRect.width()));
            textRect.setTop(textRect.top() + option.fontMetrics.height());
            for (int role : {CategoryRole, DescriptionRole, ReasonRole}) {
                const QString text = index.data(role).toString();
                if (text.isEmpty())
                    continue;
                textRect.setTop(textRect.top() + 3);
                painter->setPen(enabled && role == DescriptionRole ? QColor("#222222")
                                                                       : QColor("#777777"));
                QRect bounds;
                painter->drawText(textRect, Qt::AlignLeft | Qt::AlignTop
                    | Qt::TextWordWrap | Qt::TextWrapAnywhere, text, &bounds);
                textRect.setTop(bounds.bottom() + 1);
            }
        }
        if (option.state.testFlag(QStyle::State_HasFocus)) {
            painter->setPen(QPen(QColor("#1F4E99"), 1, Qt::DotLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(option.rect.adjusted(1, 1, -2, -2));
        }
        painter->restore();
    }

private:
    bool m_search;
};

class CatalogList final : public QListWidget {
public:
    CatalogList(CatalogView::Mode mode, QWidget *parent) : QListWidget(parent), m_mode(mode) {}

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QListWidget::resizeEvent(event);
        if (m_mode != CatalogView::Mode::Search) {
            const int availableWidth = viewport()->width();
            const int columns = qMax(1, availableWidth / 280);
            // Leave room for the style's item margins. An exact division can
            // push the last tile onto another row even when its column fits.
            setGridSize(QSize(qMax(1, (availableWidth - TextMargin) / columns),
                m_mode == CatalogView::Mode::LargeIcons ? 54 : 34));
        }
        doItemsLayout();
    }

private:
    CatalogView::Mode m_mode;
};
} // namespace

CatalogView::CatalogView(Mode mode, const QString &query, QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("catalogView"));
    setStyleSheet(QStringLiteral("#catalogView { background: #FFFFFF; }"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    const bool search = mode == Mode::Search;
    QList<ActionDefinition> actions;
    if (search) {
        for (ActionId id : ActionCatalog::search(query))
            actions.append(ActionCatalog::get(id));
    } else {
        actions = ActionCatalog::all();
    }
    if (!search) {
        actions.erase(std::remove_if(actions.begin(), actions.end(),
            [](const ActionDefinition &action) { return !action.iconItem; }), actions.end());
        std::sort(actions.begin(), actions.end(), [](const ActionDefinition &a, const ActionDefinition &b) {
            const int comparison = QString::localeAwareCompare(Branding::brand(a.title), Branding::brand(b.title));
            return comparison ? comparison < 0 : static_cast<int>(a.id) < static_cast<int>(b.id);
        });
    }

    m_list = new CatalogList(mode, this);
    m_list->setObjectName(QStringLiteral("catalogItems"));
    m_list->setAccessibleName(search ? tr("Search results") : tr("All Control Panel Items"));
    m_list->setAccessibleDescription(tr("Use the arrow keys to select a setting, then press Enter or Space to open it."));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setIconSize(QSize(mode == Mode::SmallIcons ? 16 : 32,
                              mode == Mode::SmallIcons ? 16 : 32));
    QFont listFont = m_list->font();
    listFont.setPointSize(9);
    m_list->setFont(listFont);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget#catalogItems { background: #FFFFFF; border: none; outline: none; }"
        "QListWidget#catalogItems::item:selected { background: #E5F0FB; }"
        "QListWidget#catalogItems::item:hover { background: #F2F7FC; }"));
    if (!search) {
        m_list->setViewMode(QListView::IconMode);
        m_list->setFlow(QListView::LeftToRight);
        m_list->setWrapping(true);
        m_list->setResizeMode(QListView::Adjust);
        m_list->setMovement(QListView::Static);
        m_list->setUniformItemSizes(true);
    }
    m_list->setItemDelegate(new CatalogDelegate(search, m_list));
    m_list->installEventFilter(this);
    connect(m_list, &QListWidget::itemClicked, this, &CatalogView::activateItem);

    QSet<int> seen;
    for (const ActionDefinition &action : actions) {
        const int id = static_cast<int>(action.id);
        if (seen.contains(id))
            continue;
        seen.insert(id);
        const auto availability = ActionCatalog::availability(action.id);
        const QString title = Branding::brand(action.title);
        QString description = Branding::brand(action.description);
        const QString category = Branding::brand(action.category);
        const QString reason = availability.available ? QString() :
            Branding::brand(availability.reason.isEmpty() ? tr("This setting is unavailable.") : availability.reason);
        if (description == reason)
            description.clear();
        auto *item = new QListWidgetItem(resolveIcon(action.iconName), title, m_list);
        item->setData(ActionRole, id);
        item->setData(DescriptionRole, description);
        item->setData(CategoryRole, category);
        item->setData(ReasonRole, reason);
        item->setData(Qt::AccessibleTextRole, title);
        const QString details = QStringList{category, description, reason}.join(QLatin1Char('\n')).trimmed();
        item->setData(Qt::AccessibleDescriptionRole, details);
        item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(
            (title + QLatin1Char('\n') + details).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
        item->setFlags(Qt::ItemIsSelectable | (availability.available ? Qt::ItemIsEnabled : Qt::NoItemFlags));
        if (!availability.available)
            item->setForeground(QColor("#777777"));
    }

    if (search) {
        auto *count = new QLabel(m_list->count() == 1 ? tr("1 result")
            : tr("%1 results").arg(m_list->count()), this);
        count->setObjectName(QStringLiteral("catalogResultCount"));
        count->setFont(listFont);
        count->setTextFormat(Qt::PlainText);
        count->setStyleSheet(QStringLiteral("color: #1A3C7A; background: transparent;"));
        layout->addWidget(count);
        if (m_list->count() == 0) {
            auto *empty = new QLabel(query.trimmed().isEmpty()
                ? tr("Type a name or keyword to search Control Panel.")
                : tr("No items match \"%1\". Try another search.").arg(query), this);
            empty->setObjectName(QStringLiteral("catalogNoMatches"));
            empty->setFont(listFont);
            empty->setTextFormat(Qt::PlainText);
            empty->setWordWrap(true);
            empty->setStyleSheet(QStringLiteral("color: #555555; background: transparent;"));
            layout->addWidget(empty);
        }
    }
    layout->addWidget(m_list, 1);
    setFocusProxy(m_list);
}

QSize CatalogView::sizeHint() const
{
    return QSize(700, 420);
}

bool CatalogView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Space) {
            if (key->modifiers() == Qt::NoModifier && !key->isAutoRepeat())
                activateItem(m_list->currentItem());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CatalogView::activateItem(QListWidgetItem *item)
{
    if (!item || !item->flags().testFlag(Qt::ItemIsEnabled))
        return;
    const auto id = static_cast<ActionId>(item->data(ActionRole).toInt());
    if (ActionCatalog::availability(id).available)
        emit actionActivated(id);
}
