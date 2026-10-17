#pragma once

#include <QWidget>
#include "PageId.h"

class QScrollArea;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QLabel;
class QPushButton;
class SettingsSession;
class DisplaySettings;

// Views observe one app-owned full-layout draft. All edits are staged; only
// Apply starts the independently guarded preview/Keep/Restore transaction.
class DisplayPage : public QWidget {
    Q_OBJECT
public:
    explicit DisplayPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    DisplayPage(QScrollArea *sidebar, DisplaySettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
private:
    void updateState();
    void updateArrangementControls();
    void arrangeSelected();
    void editCustomScale();
    DisplaySettings *m_session;
    QComboBox *m_outputs, *m_modes, *m_scale, *m_rotation;
    QComboBox *m_relativeOutput, *m_placement;
    QCheckBox *m_enabled;
    QSpinBox *m_x, *m_y;
    QLabel *m_monitor, *m_arrangementText, *m_changes;
    QLabel *m_status, *m_support, *m_selectedSupport, *m_confirmation;
    QPushButton *m_apply, *m_revert, *m_refresh, *m_arrange, *m_keep, *m_undoPreview;
    bool m_updating = false;
};
