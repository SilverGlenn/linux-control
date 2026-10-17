#pragma once

#include "ActionId.h"

#include <QWidget>

class QListWidget;
class QListWidgetItem;

// Catalog-only navigation surface. The owner decides how to dispatch actions.
class CatalogView : public QWidget {
    Q_OBJECT
public:
    enum class Mode { LargeIcons, SmallIcons, Search };

    explicit CatalogView(Mode mode, const QString &query = {}, QWidget *parent = nullptr);
    QSize sizeHint() const override;

signals:
    void actionActivated(ActionId id);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void activateItem(QListWidgetItem *item);
    QListWidget *m_list = nullptr;
};
