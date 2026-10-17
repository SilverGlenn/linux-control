#pragma once
#include "PageId.h"
#include <QWidget>
class QScrollArea;
class QLabel;
class QPushButton;
class QTreeWidget;
class QPlainTextEdit;
class PackageSimulationService;
class PackagePreviewPage final : public QWidget {
    Q_OBJECT
public:
    explicit PackagePreviewPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    PackagePreviewPage(QScrollArea *sidebar, PackageSimulationService *service, QWidget *parent);
    PackageSimulationService *simulationService() const { return m_service; }
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
private:
    void updateState();
    PackageSimulationService *m_service;
    QLabel *m_status, *m_message;
    QPushButton *m_start, *m_cancel;
    QTreeWidget *m_changes;
    QPlainTextEdit *m_details;
};
