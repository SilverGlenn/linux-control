#pragma once
#include "SettingsSession.h"
#include <QList>
#include <QVariantMap>

struct InputDevice {
    QString id, name, objectPath;
    QVariantMap properties;
    bool isTouchpad() const { return properties.value(QStringLiteral("touchpad")).toBool(); }
};
struct InputSnapshot {
    QString owner, version, reason;
    QList<InputDevice> devices;
    bool available = false, writable = false;
    quint64 topologyEpoch = 0;
    const InputDevice *device(const QString &id) const;
};
struct InputField {
    enum Type { Boolean, Real, Button };
    const char *key, *label, *tab, *support, *defaultKey;
    Type type;
    double minimum = 0, maximum = 1;
    bool touchpadOnly = false;
};
namespace InputTypes {
const QList<InputField> &fields();
const InputField *field(const QString &key);
bool supported(const InputDevice &device, const InputField &field);
bool validate(const InputDevice &device, const QString &key, const QVariant &value, QString &error);
bool sameDevice(const InputDevice &a, const InputDevice &b);
bool sameSnapshot(const InputSnapshot &a, const InputSnapshot &b);
// Set a whitelisted preference, maintaining KWin's mutually exclusive groups.
void setPreference(InputDevice &device, const QString &key, const QVariant &value);
QStringList coupledProperties(const QString &key);
}
Q_DECLARE_METATYPE(InputSnapshot)
enum class InputOutcome { Success, Failed, Partial, Uncertain };
Q_DECLARE_METATYPE(InputOutcome)
class InputBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent(quint64 token) = 0;
    virtual void writeChanges(quint64 token, const InputSnapshot &expected,
                              const InputSnapshot &requested) = 0;
signals:
    void readFinished(quint64 token, bool success, const InputSnapshot &snapshot, const QString &error);
    void writeFinished(quint64 token, InputOutcome outcome, const InputSnapshot &snapshot, const QString &error);
    void externalChange();
};
class InputSettings final : public SettingsSession {
    Q_OBJECT
public:
    static InputSettings *instance();
    explicit InputSettings(InputBackend *backend, QObject *parent = nullptr);
    const InputSnapshot &applied() const { return m_applied; }
    const InputSnapshot &staged() const { return m_staged; }
    bool isReady() const { return m_ready; }
    bool canApply() const { return m_ready && m_applied.writable && !m_uncertain && !m_conflict; }
    InputOutcome lastOutcome() const { return m_outcome; }
public slots:
    void selectProperty(const QString &device, const QString &key, const QVariant &value);
    void restoreDeviceDefaults(const QString &device);
    void refresh();
    void apply() override;
    void revert() override;
private:
    void read();
    void onRead(quint64 token, bool success, const InputSnapshot &snapshot, const QString &error);
    void onWritten(quint64 token, InputOutcome outcome, const InputSnapshot &snapshot, const QString &error);
    InputBackend *m_backend;
    InputSnapshot m_applied, m_staged;
    quint64 m_token = 0;
    bool m_ready = false, m_reading = false, m_writing = false, m_pendingRefresh = false, m_uncertain = false;
    InputOutcome m_outcome = InputOutcome::Failed;
    QString m_uncertainOwner;
};
