#pragma once

#include "PageId.h"
#include <QWidget>

class QScrollArea;
class QComboBox;
class QLabel;
class QPushButton;
class SettingsSession;
class AeroEffectSettings;

class AeroEffectsPage final : public QWidget {
    Q_OBJECT
public:
    explicit AeroEffectsPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    AeroEffectsPage(QScrollArea *sidebar, AeroEffectSettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
private:
    void updateState();
    AeroEffectSettings *m_session;
    QComboBox *m_strength;
    QLabel *m_status, *m_message;
    QPushButton *m_apply, *m_revert;
};
