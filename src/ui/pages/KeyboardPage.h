#pragma once

#include <QWidget>
#include <QMap>
#include "PageId.h"

class QScrollArea;
class QTabWidget;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;
class KeyboardSettings;
class SettingsSession;

class KeyboardPage final : public QWidget {
    Q_OBJECT
public:
    explicit KeyboardPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    KeyboardPage(QScrollArea *sidebar, KeyboardSettings *session, QWidget *parent = nullptr);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
private:
    void updateState();
    void updateVariants();
    void updateLayoutActions();
    void addLayout();
    void removeLayout();
    void moveLayout(int direction);
    void updateOptions(QTreeWidgetItem *item);
    void stageLabel();
    void previewLayout();
    KeyboardSettings *m_session;
    QTabWidget *m_tabs;
    QSpinBox *m_delay, *m_loop;
    QDoubleSpinBox *m_rate;
    QComboBox *m_repeat, *m_numLock, *m_model, *m_layoutChoice, *m_variantChoice, *m_policy;
    QCheckBox *m_configureLayouts, *m_resetOptions, *m_layoutOsd;
    QLineEdit *m_displayLabel;
    QTreeWidget *m_layouts, *m_options;
    QLabel *m_status, *m_runtime;
    QPushButton *m_add, *m_preview, *m_remove, *m_up, *m_down, *m_defaults, *m_refresh, *m_revert, *m_apply;
    QByteArray m_registryRevision;
    bool m_registryBuilt = false;
    bool m_updating = false;
};
