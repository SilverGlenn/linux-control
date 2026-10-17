#include "PowerProfileService.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>
#include <QCoreApplication>
#include <QPointer>
#include <QVariant>
#include <QVariantMap>

namespace {
constexpr auto kService = "net.hadess.PowerProfiles";
constexpr auto kPath = "/net/hadess/PowerProfiles";
constexpr auto kInterface = "net.hadess.PowerProfiles";

QDBusInterface ppdProperties()
{
    return QDBusInterface(QString::fromLatin1(kService),
                          QString::fromLatin1(kPath),
                          QStringLiteral("org.freedesktop.DBus.Properties"),
                          QDBusConnection::systemBus());
}

class DBusPowerProfileBackend final : public PowerProfileBackend {
public:
    using PowerProfileBackend::PowerProfileBackend;

    QStringList availableProfiles(QString *error) override
    {
        if (error)
            error->clear();
        QDBusInterface props = ppdProperties();
        if (!props.isValid()) {
            if (error)
                *error = QStringLiteral("The Power Profiles service is unavailable.");
            return {};
        }

        const QDBusReply<QVariant> reply = props.call(
            QStringLiteral("Get"), QString::fromLatin1(kInterface),
            QStringLiteral("Profiles"));
        if (!reply.isValid()) {
            if (error)
                *error = reply.error().message();
            return {};
        }

        QStringList ids;
        const QDBusArgument arg = reply.value().value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            QVariantMap entry;
            arg >> entry;
            const QString id = entry.value(QStringLiteral("Profile")).toString();
            if (!id.isEmpty())
                ids << id;
        }
        arg.endArray();
        return ids;
    }

    QString activeProfile(QString *error) override
    {
        if (error)
            error->clear();
        QDBusInterface props = ppdProperties();
        if (!props.isValid()) {
            if (error)
                *error = QStringLiteral("The Power Profiles service is unavailable.");
            return {};
        }

        const QDBusReply<QVariant> reply = props.call(
            QStringLiteral("Get"), QString::fromLatin1(kInterface),
            QStringLiteral("ActiveProfile"));
        if (!reply.isValid()) {
            if (error)
                *error = reply.error().message();
            return {};
        }
        return reply.value().toString();
    }

    void setActiveProfile(const QString &profile,
                          Completion completion) override
    {
        QDBusInterface props = ppdProperties();
        if (!props.isValid()) {
            completion(QStringLiteral("The Power Profiles service is unavailable."));
            return;
        }

        auto *watcher = new QDBusPendingCallWatcher(props.asyncCall(
            QStringLiteral("Set"), QString::fromLatin1(kInterface),
            QStringLiteral("ActiveProfile"),
            QVariant::fromValue(QDBusVariant(profile))), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [watcher, completion = std::move(completion)](
                    QDBusPendingCallWatcher *) {
            const QDBusPendingReply<> reply = *watcher;
            const QString error = reply.isError()
                ? reply.error().message() : QString();
            watcher->deleteLater();
            completion(error);
        });
    }
};
} // namespace

PowerProfileService *PowerProfileService::instance()
{
    static QPointer<PowerProfileService> service;
    if (!service) {
        service = new PowerProfileService(
            std::make_unique<DBusPowerProfileBackend>(), true,
            QCoreApplication::instance());
    }
    return service;
}

PowerProfileService::PowerProfileService(
    std::unique_ptr<PowerProfileBackend> backend, bool watchSystemBus,
    QObject *parent)
    : QObject(parent), m_backend(std::move(backend))
{
    refresh();
    if (watchSystemBus) {
        QDBusConnection::systemBus().connect(
            QString::fromLatin1(kService), QString::fromLatin1(kPath),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"), this,
            SLOT(onPropertiesChanged(QString,QVariantMap,QStringList)));
    }
}

void PowerProfileService::refresh()
{
    QString profilesError;
    QString activeError;
    m_profiles = m_backend->availableProfiles(&profilesError);
    m_activeProfile = m_backend->activeProfile(&activeError);
    m_available = profilesError.isEmpty() && !m_profiles.isEmpty();
    m_error = !profilesError.isEmpty() ? profilesError : activeError;
    emit stateChanged();
}

void PowerProfileService::setActiveProfile(const QString &profile)
{
    if (profile.isEmpty() || !m_available || m_busy
        || !m_profiles.contains(profile))
        return;

    m_busy = true;
    m_error.clear();
    m_writeError.clear();
    m_requestedProfile = profile;
    emit stateChanged();
    m_backend->setActiveProfile(profile, [this](const QString &error) {
        finishWrite(error);
    });
}

void PowerProfileService::finishWrite(const QString &writeError)
{
    QString profilesError;
    QString activeError;
    m_profiles = m_backend->availableProfiles(&profilesError);
    // The active value is deliberately read after the method reply. A successful
    // Set reply alone does not prove that the daemon now reports the requested
    // profile, and it also lets failed writes restore the real selection.
    m_activeProfile = m_backend->activeProfile(&activeError);
    m_available = profilesError.isEmpty() && !m_profiles.isEmpty();
    m_busy = false;
    m_error = !profilesError.isEmpty() ? profilesError : activeError;
    m_writeError.clear();
    if (!writeError.isEmpty())
        m_writeError = writeError;
    else if (!profilesError.isEmpty())
        m_writeError = profilesError;
    else if (!activeError.isEmpty())
        m_writeError = activeError;
    else if (m_activeProfile != m_requestedProfile)
        m_writeError = QStringLiteral(
            "Power Profiles kept “%1” active instead of applying “%2”.")
            .arg(m_activeProfile, m_requestedProfile);
    emit stateChanged();
}

void PowerProfileService::dismissError()
{
    if (m_error.isEmpty() && m_writeError.isEmpty())
        return;
    m_error.clear();
    m_writeError.clear();
    emit stateChanged();
}

void PowerProfileService::onPropertiesChanged(
    const QString &interface, const QVariantMap &changed,
    const QStringList &invalidated)
{
    if (interface != QLatin1String(kInterface))
        return;
    if (changed.contains(QStringLiteral("ActiveProfile"))
        || invalidated.contains(QStringLiteral("ActiveProfile"))
        || changed.contains(QStringLiteral("Profiles"))
        || invalidated.contains(QStringLiteral("Profiles"))) {
        refresh();
    }
}
