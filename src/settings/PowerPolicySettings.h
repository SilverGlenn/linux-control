#pragma once

#include "SettingsSession.h"
#include <QList>
#include <QMap>
#include <QVariant>
#include <functional>

enum class PowerPolicyKey {
    DisplayEnabled, DisplaySeconds, IdleAction, IdleSeconds, DimEnabled, DimSeconds,
    BrightnessEnabled, BrightnessPercent, KeyboardBrightnessEnabled, KeyboardBrightnessPercent,
    LockedDisplaySeconds, LockBeforeDisplayOff, PowerButtonAction, PowerDownAction,
    LidAction, LidExternalMonitorInhibition, SleepMode,
    BatteryLowLevel, BatteryCriticalLevel, BatteryCriticalAction
};
enum class PowerPolicyIntent { Set, RestoreDefault };
struct PowerPolicyKeyState {
    bool valueKnown = false, defaultKnown = false;
    bool effectivePresent = false, localPresent = false, inheritedPresent = false, globalPresent = false;
    bool immutable = false, writable = false, resettable = false;
    QVariant defaultValue;
    QString reason, defaultReason, resetReason;
    QByteArray revision, inheritedRevision, globalRevision;
    bool operator==(const PowerPolicyKeyState &other) const;
};

struct PowerPolicyAction {
    uint value = 0;
    QString label;
    bool operator==(const PowerPolicyAction &other) const { return value == other.value && label == other.label; }
};

// Raw PowerDevil values are retained even when the UI cannot edit them. In
// particular, Hibernate/Shutdown actions are never translated into Sleep.
struct PowerPolicyProfile {
    QString id;
    bool displayEnabled = true;
    int displaySeconds = 600;
    uint suspendAction = 0;
    int suspendSeconds = 900;
    bool displayEditable = false;
    bool sleepEditable = false;
    QString displayReason;
    QString sleepReason;
    QList<PowerPolicyAction> idleActions, powerButtonActions, lidActions, sleepModes;
    bool dimEnabled = false;
    int dimSeconds = 300;
    bool brightnessEnabled = false;
    int brightnessPercent = 100;
    bool keyboardBrightnessEnabled = false;
    int keyboardBrightnessPercent = 100;
    int lockedDisplaySeconds = 60;
    bool lockBeforeDisplayOff = false;
    uint powerButtonAction = 0, powerDownAction = 0, lidAction = 0, sleepMode = 1;
    bool inhibitLidActionWhenExternalMonitorPresent = false;
    bool dimEditable = false, brightnessEditable = false, keyboardBrightnessEditable = false;
    bool lockedDisplayEditable = false, lockBeforeDisplayOffEditable = false;
    bool powerButtonEditable = false, powerDownEditable = false, lidEditable = false;
    bool lidExternalMonitorEditable = false, sleepModeEditable = false;
    QString dimReason, brightnessReason, keyboardBrightnessReason;
    QString lockedDisplayReason, lockBeforeDisplayOffReason;
    QString powerButtonReason, powerDownReason, lidReason, lidExternalMonitorReason, sleepModeReason;
    QMap<PowerPolicyKey, PowerPolicyKeyState> keys;
    QMap<PowerPolicyKey, PowerPolicyIntent> intents;
    bool operator==(const PowerPolicyProfile &other) const;
};
struct PowerPolicySnapshot {
    QList<PowerPolicyProfile> profiles;
    bool serviceAvailable = false;
    QString unavailableReason;
    QString owner, capabilityRevision;
    int batteryLowLevel = 10, batteryCriticalLevel = 5;
    uint batteryCriticalAction = 0;
    bool batteryLevelsEditable = false, batteryCriticalActionEditable = false;
    QString batteryLevelsReason, batteryCriticalActionReason;
    QList<PowerPolicyAction> batteryCriticalActions;
    QMap<PowerPolicyKey, PowerPolicyKeyState> keys;
    QMap<PowerPolicyKey, PowerPolicyIntent> intents;
    bool operator==(const PowerPolicySnapshot &other) const;
    bool operator!=(const PowerPolicySnapshot &other) const { return !(*this == other); }
};
Q_DECLARE_METATYPE(PowerPolicySnapshot)

class PowerPolicyBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent() = 0;
    virtual void writePolicy(const PowerPolicySnapshot &expected,
                             const PowerPolicySnapshot &requested) = 0;
    virtual void cancel() = 0;
signals:
    void readFinished(bool success, const PowerPolicySnapshot &snapshot, const QString &error);
    // saved and reloadAcknowledged are deliberately independent. A failed or
    // cancelled reload can leave the file saved without a live acknowledgement.
    void writeFinished(bool saved, bool reloadAcknowledged, bool cancelled, const QString &error);
    void externalChange();
};

class PowerPolicySettings final : public SettingsSession {
    Q_OBJECT
public:
    static PowerPolicySettings *instance();
    explicit PowerPolicySettings(PowerPolicyBackend *backend, QObject *parent = nullptr);
    bool isReady() const { return m_ready; }
    const PowerPolicySnapshot &applied() const { return m_applied; }
    const PowerPolicySnapshot &staged() const { return m_staged; }
    bool wasCancelled() const { return m_cancelled; }
    static constexpr int MaximumTimeoutSeconds = 604800;
    enum class DefaultsScope { SelectedProfile, AllProfiles, Battery, AllPolicies };
    struct DefaultsReview {
        QStringList changes, unchanged, unavailable;
        QByteArray revision;
        bool hasChanges() const { return !changes.isEmpty(); }
    };
    DefaultsReview reviewDefaults(DefaultsScope scope, const QString &profile = {}) const;
    QString applySupportText() const;
public slots:
    // -1 means Never; otherwise seconds, at least 30 for display / 60 for sleep.
    void selectDisplayTimeout(const QString &profile, int seconds);
    void selectSleepTimeout(const QString &profile, int seconds);
    void selectIdleAction(const QString &profile, uint action);
    void selectIdleTimeout(const QString &profile, int seconds);
    void selectDimTimeout(const QString &profile, int seconds);
    void selectBrightness(const QString &profile, bool enabled, int percent);
    void selectKeyboardBrightness(const QString &profile, bool enabled, int percent);
    void selectLockedDisplayTimeout(const QString &profile, int seconds);
    void selectLockBeforeDisplayOff(const QString &profile, bool enabled);
    void selectPowerButtonAction(const QString &profile, uint action);
    void selectPowerDownAction(const QString &profile, uint action);
    void selectLidAction(const QString &profile, uint action);
    void selectLidExternalMonitorInhibition(const QString &profile, bool enabled);
    void selectSleepMode(const QString &profile, uint mode);
    void selectBatteryLevels(int low, int critical);
    void selectBatteryCriticalAction(uint action);
    void selectValue(const QString &profile, PowerPolicyKey key, const QVariant &value);
    void selectGlobalValue(PowerPolicyKey key, const QVariant &value);
    void restoreKeyDefault(const QString &profile, PowerPolicyKey key);
    void restoreGlobalKeyDefault(PowerPolicyKey key);
    void stageDefaults(DefaultsScope scope, const QString &profile, const QByteArray &reviewRevision);
    void apply() override;
    void revert() override;
    void refresh();
    void cancel();
private:
    enum class Phase { Idle, Refreshing, Preflight, Writing, Readback };
    void selectTimeout(const QString &profile, int seconds, bool display);
    void editProfile(const QString &profile, const std::function<bool(PowerPolicyProfile &)> &edit);
    void edited();
    void startRefresh();
    void readFinished(bool success, const PowerPolicySnapshot &snapshot, const QString &error);
    void writeFinished(bool saved, bool acknowledged, bool cancelled, const QString &error);
    void finish(bool applying = false, bool success = false);
    PowerPolicyBackend *m_backend;
    Phase m_phase = Phase::Idle;
    PowerPolicySnapshot m_applied, m_staged, m_requested;
    bool m_ready = false;
    bool m_refreshPending = false;
    bool m_saved = false;
    bool m_acknowledged = false;
    bool m_cancelled = false;
    bool m_applyError = false;
    bool m_reverting = false;
    QString m_writeError;
};
