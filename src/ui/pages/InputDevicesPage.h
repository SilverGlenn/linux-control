#pragma once
#include <QWidget>
#include <QMap>
#include <QVariant>
#include "PageId.h"
class QScrollArea;
class QComboBox;
class QLabel;
class QPushButton;
class QTabWidget;
class InputSettings;
class SettingsSession;
class InputDevicesPage final : public QWidget {
    Q_OBJECT
public:
    explicit InputDevicesPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    InputDevicesPage(QScrollArea *sidebar, InputSettings *session, QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
private:
    void updateState();
    void stage(const QString &key, const QVariant &value);
    void updateModes(QComboBox *combo, const QStringList &keys, bool allowNone);
    InputSettings *m_session;
    QComboBox *m_devices, *m_acceleration, *m_clickMethod, *m_scrollMethod;
    QTabWidget *m_tabs;
    QLabel *m_status, *m_details;
    QPushButton *m_apply, *m_revert, *m_defaults, *m_refresh;
    QMap<QString, QWidget *> m_controls, m_rows;
    QString m_selected;
};
