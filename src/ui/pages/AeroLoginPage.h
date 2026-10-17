#pragma once
#include "PageId.h"
#include "settings/AeroLoginSettings.h"
#include <QPointer>
#include <QWidget>
class QCheckBox;
class QEvent;
class QLabel;
class QPushButton;
class QScrollArea;

// Only explicit choices stage data. The application-owned session retains the
// canonical image bytes and any prepared selection across view recreation.
class AeroLoginPage final : public QWidget {
    Q_OBJECT
public:
    explicit AeroLoginPage(QScrollArea *sidebar,QWidget *parent=nullptr);
    AeroLoginPage(QScrollArea *sidebar,AeroLoginSettings *session,QWidget *parent);
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
signals:
    void navigationRequested(PageId page);
protected:
    bool eventFilter(QObject *object,QEvent *event) override;
private:
    void updateState();
    void chooseImage();
    void prepareSelection(const QString &path);
    void reviewPreparedImage();
    void reviewApply();
    void reviewDefaults();
    void reviewRecovery();
    bool currentContext(const AeroLoginSnapshot &saved,const AeroLoginSnapshot &draft) const;
    QPointer<AeroLoginSettings> m_session;
    QMap<AeroLoginKey,QCheckBox *> m_fields;
    QMap<AeroLoginKey,QLabel *> m_fieldWarnings;
    QLabel *m_loginPreview=nullptr,*m_lockPreview=nullptr,*m_imageSummary=nullptr,*m_effects=nullptr;
    QLabel *m_pending=nullptr,*m_changes=nullptr,*m_message=nullptr,*m_detailsText=nullptr;
    QWidget *m_details=nullptr;
    QPushButton *m_choose=nullptr,*m_remove=nullptr,*m_restore=nullptr,*m_reviewImage=nullptr;
    QPushButton *m_defaults=nullptr,*m_refresh=nullptr,*m_revert=nullptr,*m_apply=nullptr,*m_recover=nullptr;
    QString m_inputMessage;
    bool m_updating=false,m_chooserQueued=false,m_waitRevert=false;
};
