#include "CategoryWidget.h"
#include "IconHelper.h"
#include "Branding.h"
#include "LinkLabel.h"
#include "ActionCatalog.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>

CategoryWidget::CategoryWidget(const CategoryItem &item, QWidget *parent)
    : QWidget(parent)
    , m_title(item.title)
{
    setContentsMargins(0, 0, 0, 0);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(10, 4, 10, 4);
    root->setSpacing(14);
    root->setAlignment(Qt::AlignTop);

    // Icon
    auto *iconLabel = new QLabel;
    iconLabel->setPixmap(resolveIcon(item.iconName).pixmap(48, 48));
    iconLabel->setFixedSize(48, 48);
    iconLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    root->addWidget(iconLabel, 0, Qt::AlignTop);

    // Text block
    auto *textBlock = new QVBoxLayout;
    textBlock->setSpacing(0);
    textBlock->setContentsMargins(0, 0, 0, 0);

    // Display the branded wording; m_title stays canonical for routing.
    auto *titleLabel = new LinkLabel(Branding::brand(item.title));
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(11);
    titleLabel->setFont(titleFont);
    titleLabel->setCursor(Qt::PointingHandCursor);
    titleLabel->setStyleSheet(
        "QLabel { color: #1E7B1E; }"
        "QLabel:hover { color: #145214; }"
    );
    connect(titleLabel, &LinkLabel::clicked, this, [this] {
        emit titleActivated(m_title);
    });
    textBlock->addWidget(titleLabel);

    for (ActionId actionId : item.tasks) {
        const auto &action = ActionCatalog::get(actionId);
        auto *taskLabel = new LinkLabel(Branding::brand(action.title));
        taskLabel->setAccessibleDescription(Branding::brand(action.description));
        m_taskLabels.insert(taskLabel, actionId);
        connect(taskLabel, &LinkLabel::clicked, this, [this, actionId] {
            emit actionActivated(actionId);
        });
        textBlock->addWidget(taskLabel);
        const auto availability = ActionCatalog::availability(actionId);
        setTaskAvailable(actionId, availability.available, availability.reason);
    }

    root->addLayout(textBlock, 1);
    setLayout(root);
}

void CategoryWidget::setTaskAvailable(ActionId action, bool available, const QString &reason)
{
    for (auto it = m_taskLabels.constBegin(); it != m_taskLabels.constEnd(); ++it) {
        if (it.value() != action)
            continue;
        auto *label = it.key();
        label->setEnabled(available);
        const QString unavailableReason = reason.isEmpty()
            ? QStringLiteral("This setting is not available in Control Panel yet.") : reason;
        label->setToolTip(available ? QString() : unavailableReason);
        label->setAccessibleDescription(available
            ? Branding::brand(ActionCatalog::get(action).description) : unavailableReason);
    }
}
