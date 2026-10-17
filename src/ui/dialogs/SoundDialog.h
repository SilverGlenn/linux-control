#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class AudioSettings;
struct AudioObject;
class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QComboBox;
class QTabWidget;
class QVBoxLayout;

// A native Windows 7 audio view. All reads and immediate changes belong to the
// application-owned AudioSettings session, so closing a view cannot cancel a job.
class SoundDialog : public QDialog {
    Q_OBJECT
public:
    explicit SoundDialog(QWidget *parent = nullptr);
    explicit SoundDialog(AudioSettings *settings, QWidget *parent = nullptr);
    enum Tab { TabPlayback = 0, TabRecording = 1,
               TabSounds = 2, TabCommunications = 3, TabApplications = 4 };
    void showTab(Tab tab);

private:
    struct ObjectEditor {
        QListWidget *list = nullptr;
        QLabel *details = nullptr;
        QSpinBox *volume = nullptr;
        QPushButton *setVolume = nullptr;
        QCheckBox *mute = nullptr;
        QPushButton *setDefault = nullptr;
        QPushButton *properties = nullptr;
        QComboBox *route = nullptr;
        QPushButton *setRoute = nullptr;
        QString selectedId, renderedId, confirmedRoute;
        QVector<quint32> confirmedVolumes;
    };
    QWidget *buildDeviceTab(ObjectEditor &editor, bool playback);
    QWidget *buildApplicationsTab();
    QWidget *buildUnavailableTab(const QString &text);
    void addVolumeEditor(QVBoxLayout *layout, ObjectEditor &editor, const QString &prefix);
    void updateView();
    void updateList(ObjectEditor &editor, int category);
    void updateEditor(ObjectEditor &editor, bool playback, bool applications = false);
    const AudioObject *selectedObject(const ObjectEditor &editor) const;
    void showProperties(ObjectEditor &editor);
    void setVolume(ObjectEditor &editor);
    void setMute(ObjectEditor &editor, bool mute);
    void setDefault(ObjectEditor &editor);
    void setRoute(ObjectEditor &editor);

    AudioSettings *m_settings = nullptr;
    QTabWidget *m_tabs = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_refresh = nullptr;
    ObjectEditor m_playback, m_recording, m_applications;
};
