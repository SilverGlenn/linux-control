#pragma once
#include "PageId.h"
#include "settings/NightTimeSettings.h"
#include "platform/location/NightTimeLocation.h"
#include <QPointer>
#include <QWidget>

class QComboBox;
class QGroupBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QScrollArea;
class NightTimeMapWidget;
class NightTimePreviewWidget;

// Preferences live in the application-owned session. This view owns only a
// cancellable, explicit one-shot location request and its unsaved sample.
class NightTimePage final : public QWidget {
    Q_OBJECT
public:
    explicit NightTimePage(QScrollArea *sidebar, QWidget *parent = nullptr);
    NightTimePage(QScrollArea *sidebar, NightTimeSettings *session, NightTimeLocation *location, QWidget *parent);
    ~NightTimePage() override;
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
protected:
    void hideEvent(QHideEvent *event) override;
private:
    struct Editor {
        NightTimeKey key;
        QWidget *control = nullptr;
        QLineEdit *line = nullptr;
        QLabel *description = nullptr;
        bool active = false, deferred = false, commit = false, staged = false;
        QString text;
        QVariant candidate;
        int cursor = 0, selectionStart = -1, selectionLength = 0;
        NightTimeSnapshot baseline, expected;
    };
    void updateState();
    bool inUse(NightTimeKey key) const;
    bool contextCurrent(const Editor &editor) const;
    void input(Editor &editor, const QString &text);
    void stageInput(Editor &editor);
    void commitInput(Editor &editor);
    bool finishInputs();
    void clearInputs();
    void defaultAction(QWidget *control, NightTimeKey key);
    void requestLocation();
    void cancelLocation();
    void updateForecast();
    QPointer<NightTimeSettings> m_session;
    QPointer<NightTimeLocation> m_location;
    QComboBox *m_source = nullptr;
    QRadioButton *m_automatic = nullptr, *m_manual = nullptr;
    QGroupBox *m_locationGroup = nullptr, *m_timesGroup = nullptr;
    Editor m_latitude, m_longitude, m_sunrise, m_sunset, m_duration;
    QList<Editor *> editors();
    NightTimeMapWidget *m_map = nullptr;
    NightTimePreviewWidget *m_preview = nullptr;
    QLabel *m_forecast = nullptr, *m_privacy = nullptr, *m_locationStatus = nullptr;
    QLabel *m_inputs = nullptr, *m_changes = nullptr, *m_message = nullptr, *m_defaultsReview = nullptr;
    QPushButton *m_acquire = nullptr, *m_cancel = nullptr, *m_use = nullptr;
    QPushButton *m_defaults = nullptr, *m_refresh = nullptr, *m_revert = nullptr, *m_apply = nullptr;
    bool m_updating = false, m_ownCommit = false, m_requestActive = false, m_haveFix = false;
    quint64 m_locationToken = 0;
    NightTimeSnapshot m_locationBaseline, m_locationDraft;
    NightTimeLocationFix m_fix;
    QString m_inputMessage, m_locationMessage;
};
