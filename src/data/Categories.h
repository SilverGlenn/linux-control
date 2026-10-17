#pragma once

#include <QString>
#include <QStringList>
#include <QList>

#include "ActionId.h"

// A top-level Control Panel category as shown on the home page grid.
struct CategoryItem {
    QString     iconName;
    QString     title;
    QList<ActionId> tasks;
};

// A heading and its task links within a category's detail page.
struct DetailGroup {
    ActionId action = ActionId::None;
    QList<QList<ActionId>> lines;   // each line is a row of links
};

// Categories shown in the home page two-column grid.
const QList<CategoryItem> &homeCategories();

// Category order in the left-nav pane of detail pages.
const QStringList &navOrder();

// Detail-page groups for a category, or nullptr if it has no detail page yet.
const QList<DetailGroup> *detailGroupsFor(const QString &category);
