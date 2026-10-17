#pragma once

#include "ActionId.h"
#include "PageId.h"

#include <QList>
#include <QString>
#include <QStringList>

struct ActionDefinition {
    ActionId id;
    QString title;
    QString description;
    QString iconName;
    QString category;
    LinkTarget target;
    QStringList keywords;
    bool iconItem;
};

namespace ActionCatalog {
struct Availability {
    bool available;
    QString reason;
};

const QList<ActionDefinition> &all();
const ActionDefinition &get(ActionId id);
QList<ActionId> search(const QString &query);
Availability availability(ActionId id);
ActionId idForTarget(const LinkTarget &target);
Availability availability(const LinkTarget &target);
} // namespace ActionCatalog
