#pragma once
#include "PageId.h"
#include "settings/ScreenLockSettings.h"
#include "platform/kde/ScreenLockAppearance.h"
#include <QPointer>
#include <QWidget>
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;

// The application owns the persistent draft. Appearance evidence is a passive,
// read-only value: constructing an injected view never discovers providers.
class ScreenLockPage final : public QWidget {
    Q_OBJECT
public:
    explicit ScreenLockPage(QScrollArea *sidebar,QWidget *parent=nullptr);
    ScreenLockPage(QScrollArea *sidebar,ScreenLockSettings *session,const ScreenLockAppearance::Evidence &appearance,QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
signals:
    void editLockShortcutRequested();
    void appearanceRequested();
private:
    void updateState();
    void activateTimeout(int index);
    void activateGrace(int index);
    void customInterval(bool grace);
    void queueChoice(bool grace,int choice);
    void reviewDefaults();
    void defaultAction(QWidget *control,ScreenLockKey key);
    bool currentContext(const ScreenLockSnapshot &saved,const ScreenLockSnapshot &draft) const;
    QPointer<ScreenLockSettings> m_session;
    ScreenLockAppearance::Evidence m_appearance;
    QComboBox *m_timeout=nullptr,*m_grace=nullptr;
    QCheckBox *m_resume=nullptr;
    QLabel *m_timeoutDescription=nullptr,*m_graceDescription=nullptr,*m_effect=nullptr;
    QLabel *m_saved=nullptr,*m_changes=nullptr,*m_message=nullptr,*m_appearanceText=nullptr;
    QLabel *m_appearanceSummary=nullptr,*m_configuration=nullptr;
    QWidget *m_details=nullptr;
    QPushButton *m_defaults=nullptr,*m_refresh=nullptr,*m_revert=nullptr,*m_apply=nullptr;
    QString m_inputMessage;
    bool m_updating=false,m_dialogPending=false;
};
