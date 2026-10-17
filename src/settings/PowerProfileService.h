#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <memory>

class PowerProfileBackend : public QObject {
public:
    using Completion = std::function<void(const QString &error)>;

    explicit PowerProfileBackend(QObject *parent = nullptr)
        : QObject(parent) {}
    ~PowerProfileBackend() override = default;

    virtual QStringList availableProfiles(QString *error) = 0;
    virtual QString activeProfile(QString *error) = 0;
    virtual void setActiveProfile(const QString &profile,
                                  Completion completion) = 0;
};

// Application-owned access to power-profiles-daemon. The service tracks writes
// independently of any page so navigation cannot discard their completion or
// error state.
class PowerProfileService : public QObject {
    Q_OBJECT
public:
    static PowerProfileService *instance();

    // Backend injection keeps lifecycle behavior testable without a system bus.
    explicit PowerProfileService(std::unique_ptr<PowerProfileBackend> backend,
                                 bool watchSystemBus = false,
                                 QObject *parent = nullptr);

    QStringList availableProfiles() const { return m_profiles; }
    QString activeProfile() const { return m_activeProfile; }
    bool isAvailable() const { return m_available; }
    bool isBusy() const { return m_busy; }
    QString error() const
    {
        return m_writeError.isEmpty() ? m_error : m_writeError;
    }

    void refresh();
    void setActiveProfile(const QString &profile);
    void dismissError();

signals:
    void stateChanged();

private slots:
    void onPropertiesChanged(const QString &interface,
                             const QVariantMap &changed,
                             const QStringList &invalidated);

private:
    void finishWrite(const QString &writeError);

    std::unique_ptr<PowerProfileBackend> m_backend;
    QStringList m_profiles;
    QString m_activeProfile;
    QString m_error;
    QString m_writeError;
    QString m_requestedProfile;
    bool m_available = false;
    bool m_busy = false;
};
