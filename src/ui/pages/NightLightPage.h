#pragma once

#include "PageId.h"
#include "settings/NightLightSettings.h"
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class NightLightSettings;
class SettingsSession;
enum class NightLightKey;

// The app owns the draft and Apply transaction. This view owns only its explicit
// temporary preview, which it releases when hidden or destroyed.
class NightLightPage final : public QWidget {
    Q_OBJECT
public:
    explicit NightLightPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    NightLightPage(QScrollArea *sidebar, NightLightSettings *session, QWidget *parent);
    ~NightLightPage() override;
    SettingsSession *settingsSession() const;
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();
signals:
    void navigationRequested(PageId page);
protected:
    void hideEvent(QHideEvent *event) override;
private:
    struct TemperatureControls {
        NightLightKey key;
        QSpinBox *value = nullptr;
        QSlider *slider = nullptr;
        QLabel *description = nullptr;
        QPushButton *preview = nullptr;
        bool inputActive = false, textInput = false, gesture = false, deferred = false, commitRequested = false, candidateStaged = false;
        QString pendingText;
        QVariant pendingValue;
        int cursor = -1, selectionStart = -1, selectionLength = 0;
        NightLightSnapshot baseline, expectedDraft;
    };
    void updateState();
    bool beginTemperatureInput(TemperatureControls &controls);
    bool inputContextCurrent(const TemperatureControls &controls) const;
    void stageTemperatureInput(TemperatureControls &controls);
    void commitTemperatureInput(TemperatureControls &controls);
    void rejectTemperatureInput(TemperatureControls &controls, const QString &reason);
    void clearTemperatureInput(TemperatureControls &controls);
    bool finishTemperatureInputs();
    void releasePreview();
    void installDefaultAction(QWidget *control, NightLightKey key);
    QPointer<NightLightSettings> m_session;
    QCheckBox *m_enabled = nullptr;
    QComboBox *m_mode = nullptr;
    TemperatureControls m_day, m_night;
    QLabel *m_saved = nullptr, *m_changes = nullptr, *m_runtime = nullptr;
    QLabel *m_previous = nullptr, *m_scheduled = nullptr, *m_previewStatus = nullptr;
    QLabel *m_defaultsReview = nullptr, *m_message = nullptr;
    QPushButton *m_defaults = nullptr, *m_stop = nullptr, *m_refresh = nullptr, *m_revert = nullptr, *m_apply = nullptr;
    bool m_updating = false, m_previewReleaseRequested = false, m_ownInputCommit = false;
    QString m_inputMessage;
};
