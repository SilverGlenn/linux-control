#pragma once
#include <QWidget>
#include "PageId.h"
class WallpaperSettings;
class SettingsSession;
class QScrollArea;
class QLabel;
class QComboBox;
class QPushButton;
class DesktopBackgroundPage final : public QWidget {
    Q_OBJECT
public:
    explicit DesktopBackgroundPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    DesktopBackgroundPage(QScrollArea *sidebar, WallpaperSettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
private:
    void refresh();
    WallpaperSettings *m_session;
    QComboBox *m_desktop, *m_position;
    QLabel *m_preview, *m_path, *m_status;
    QPushButton *m_browse, *m_apply, *m_revert;
    QString m_previewSource;
};
