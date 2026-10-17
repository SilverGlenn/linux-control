#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include "PageId.h"
#include <QList>
#include <QHash>

#include "settings/PowerProfileService.h"
#include "settings/PowerPolicySettings.h"

class QScrollArea;
class QVBoxLayout;
class QButtonGroup;
class QAbstractButton;
class QRadioButton;
class QLabel;
class QObject;
class QEvent;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QGroupBox;
class QPushButton;

// The "Power Options" detail page, a Linux-flavoured take on the Windows 7
// "Select a power plan" screen.
//
// On machines that run power-profiles-daemon (PPD), the same service KDE's and
// GNOME's power widgets drive, the radio plans are wired live to it over the
// system bus:
//   * Reading  net.hadess.PowerProfiles.Profiles / .ActiveProfile lists the
//     available plans and the active one.
//   * Selecting a plan writes .ActiveProfile (polkit lets the active session do
//     this without a password, exactly like `powerprofilesctl set`).
//   * A PropertiesChanged subscription keeps the selection in sync when the
//     profile is changed elsewhere (KDE tray, another app, on AC/battery).
//
// PPD's three standard profiles map onto the three Windows plans:
//   performance  -> "High performance"
//   balanced     -> "Balanced (recommended)"
//   power-saver  -> "Power saver"
//
// When PPD is absent, the page shows a disabled Balanced placeholder explaining
// that no active profile can be read or changed.
class PowerOptionsPage : public QWidget {
    Q_OBJECT

public:
    explicit PowerOptionsPage(QScrollArea *sidebar, QWidget *parent = nullptr);
    PowerOptionsPage(QScrollArea *sidebar, PowerPolicySettings *session, QWidget *parent = nullptr);
    PowerOptionsPage(QScrollArea *sidebar, PowerPolicySettings *session,
                     PowerProfileService *profiles, QWidget *parent);
    SettingsSession *settingsSession() const { return m_policy; }

    // Left-nav entries shown by MainWindow's subpage sidebar.
    static QList<SidebarLink> sidebarLinks();
    static QList<SidebarLink> sidebarSeeAlso();

protected:
    // Lets a click on a plan's name label select that plan, like Windows.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // One plan row. `profileId` is the raw PPD profile name used to read/write
    // the daemon; empty for a disabled placeholder when PPD is unavailable.
    struct Plan {
        QString profileId;
        QString name;          // e.g. "Balanced (recommended)"
        QString description;   // the grey line under the name
        bool    additional;    // true -> lives under "Show additional plans"
    };

    // Query PPD over the system bus. Returns the profiles it advertises (in the
    // daemon's own low-to-high order) and the active one. `ok` is false when PPD is
    // not reachable, which triggers the static fall-back layout.
    QStringList availableProfiles(bool &ok) const;
    QString     activeProfile() const;
    void        setActiveProfile(const QString &profileId);
    void        setProfileControlsEnabled(bool enabled);
    void        updateServiceState();
    void buildPolicyControls(QVBoxLayout *layout);
    void buildAdvancedControls(QVBoxLayout *layout);
    void updateAdvancedState(bool enabled);
    QString selectedPolicySource() const;
    void updatePolicyState();
    void bindPolicyControl(QWidget *control, PowerPolicyKey key, const QString &source);
    void activatePolicyChoice(QComboBox *control, int index);
    void editCustomTimeout(QComboBox *control);
    void showDefaultsReview();
    QString controlSource(QWidget *control) const;
    void stageControlValue(QWidget *control, const QVariant &value);
    void restoreControlDefault(QWidget *control);

    // Build the visual plan list and the additional-plans expander.
    void buildPlans(const QStringList &profiles);
    QWidget *buildPlanRow(const Plan &plan);

    // Re-checks the radio matching the service's most recently read ActiveProfile.
    void syncSelection();

private:
    QVBoxLayout  *m_planList      = nullptr;   // holds the preferred-plan rows
    QVBoxLayout  *m_additionalBox = nullptr;   // holds the additional-plan rows
    QWidget      *m_additionalWrap = nullptr;  // toggled by the expander
    QWidget      *m_additionalHeader = nullptr; // heading row, hidden if empty
    QButtonGroup *m_group         = nullptr;
    QLabel       *m_errorLabel    = nullptr;
    PowerProfileService *m_service = nullptr;
    PowerPolicySettings *m_policy = nullptr;
    struct TimeoutControls {
        QString profile;
        QCheckBox *displayEnabled;
        QComboBox *display;
        QComboBox *sleep;
        QLabel *reason;
    };
    QList<TimeoutControls> m_timeoutControls;
    QLabel *m_policyStatus = nullptr;
    QPushButton *m_apply = nullptr;
    QPushButton *m_revert = nullptr;
    QPushButton *m_advancedToggle = nullptr;
    QGroupBox *m_advanced = nullptr;
    QComboBox *m_policySource = nullptr;
    QComboBox *m_idleAction = nullptr, *m_dim = nullptr, *m_lockedDisplay = nullptr;
    QComboBox *m_powerButton = nullptr, *m_powerDown = nullptr, *m_lid = nullptr, *m_sleepMode = nullptr;
    QComboBox *m_criticalAction = nullptr;
    QCheckBox *m_dimEnabled = nullptr;
    QCheckBox *m_brightnessEnabled = nullptr, *m_keyboardBrightnessEnabled = nullptr;
    QCheckBox *m_lockDisplay = nullptr, *m_inhibitLid = nullptr;
    QSpinBox *m_brightness = nullptr, *m_keyboardBrightness = nullptr;
    QSpinBox *m_batteryLow = nullptr, *m_batteryCritical = nullptr;
    QLabel *m_advancedReason = nullptr;
    QPushButton *m_defaults = nullptr;
    bool m_updatingPolicy = false;
    bool          m_ppdAvailable  = false;
    bool          m_syncing       = false;     // guards programmatic re-checks

    // Maps each plan's name label to its radio, so clicking the name selects it.
    QHash<QObject *, QRadioButton *> m_nameToRadio;
};
