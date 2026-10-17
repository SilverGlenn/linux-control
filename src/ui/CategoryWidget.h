#pragma once

#include <QWidget>
#include <QString>
#include <QHash>

#include "Categories.h"
#include "ActionCatalog.h"

class LinkLabel;

class CategoryWidget : public QWidget {
    Q_OBJECT

public:
    explicit CategoryWidget(const CategoryItem &item, QWidget *parent = nullptr);

    void setTaskAvailable(ActionId action, bool available, const QString &reason = {});

Q_SIGNALS:
    void titleActivated(const QString &category);
    void actionActivated(ActionId action);

private:
    QString m_title;
    QHash<LinkLabel *, ActionId> m_taskLabels;
};
