#pragma once

#include <QWidget>
#include <QHash>
#include <QList>
#include "PageId.h"

class QScrollArea;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class UserFontService;

class FontsPage : public QWidget {
    Q_OBJECT
public:
    explicit FontsPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    FontsPage(QScrollArea *sidebar, UserFontService *service, QWidget *parent = nullptr);
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void rebuild();
    void updateState();
    void selectRow(int index);
    void openSelected();
    void removeSelected();
    UserFontService *m_service;
    QGridLayout *m_grid;
    QScrollArea *m_scroll;
    QLineEdit *m_search;
    QLabel *m_status;
    QLabel *m_details;
    QPushButton *m_refresh;
    QPushButton *m_preview;
    QPushButton *m_install;
    QPushButton *m_remove;
    int m_selected = -1;
    QHash<QObject *, int> m_rowToIndex;
    QList<QWidget *> m_rows;
    QList<int> m_visible;
};
