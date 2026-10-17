#pragma once
#include <QWidget>
#include "PageId.h"
#include <array>
class QScrollArea;
class QLabel;
class QPushButton;
class QComboBox;
class FontSettings;
class SettingsSession;
class FontSettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit FontSettingsPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    FontSettingsPage(QScrollArea *sidebar, FontSettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
private:
    void updateState();
    FontSettings *m_session;
    std::array<QPushButton *, 6> m_fonts{};
    QComboBox *m_antialias, *m_hinting, *m_subpixel;
    QLabel *m_status, *m_message;
    QPushButton *m_apply, *m_revert;
};
