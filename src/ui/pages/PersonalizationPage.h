#pragma once
#include "PageId.h"
#include <QWidget>
#include <QHash>
#include <QList>
class QScrollArea;
class QLabel;
class QPushButton;
class QSlider;
class SettingsSession;
class AeroThemeSettings;
class PersonalizationPage final : public QWidget {
    Q_OBJECT
public:
    explicit PersonalizationPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    PersonalizationPage(QScrollArea *sidebar, AeroThemeSettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
signals:
    void navigationRequested(PageId page);
private:
    void updateState();
    AeroThemeSettings *m_session;
    QHash<QString, QWidget *> m_controls;
    QHash<QString, QLabel *> m_saved;
    QHash<QString, QSlider *> m_sliders;
    QList<QPushButton *> m_colors;
    QLabel *m_status, *m_message, *m_review, *m_preview;
    QPushButton *m_apply, *m_revert, *m_restore;
};
