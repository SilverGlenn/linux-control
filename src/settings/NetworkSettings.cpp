#include "NetworkSettings.h"
#include "platform/network/NetworkManagerBackend.h"

#include <QCoreApplication>
#include <QPointer>

NetworkSettings *NetworkSettings::instance()
{
    static QPointer<NetworkSettings> session;
    if (!session) session = new NetworkSettings(new NetworkManagerBackend, QCoreApplication::instance());
    return session;
}

NetworkSettings::NetworkSettings(NetworkBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    NetworkTypes::registerDbusTypes();
    backend->setParent(this);
    connect(backend, &NetworkBackend::readFinished, this, &NetworkSettings::readDone);
    connect(backend, &NetworkBackend::transactionFinished, this, &NetworkSettings::finish);
    connect(backend, &NetworkBackend::externalChange, this, [this] {
        if (m_busy || m_scanning) m_refreshPending = true;
        else refresh();
    });
    connect(backend, &NetworkBackend::scanFinished, this, [this](bool success, const QString &error) {
        m_scanning = false;
        if (!success) m_error = error.isEmpty() ? QStringLiteral("The Wi-Fi scan failed.") : error;
        emit stateChanged();
        if (success || m_refreshPending) {
            if (m_busy) m_refreshPending = true;
            else refresh();
        }
    });
    connect(backend, &NetworkBackend::previewReady, this, [this](const NetworkSnapshot &snapshot, int seconds) {
        if (m_phase != Phase::Preview || m_resolutionPending) return;
        if (!NetworkTypes::matchesRequest(m_baseline, snapshot, m_request)) {
            m_error = QStringLiteral("The requested network change could not be verified. Restoring the previous connection…");
            m_resolutionPending = true;
            m_backend->revertChange();
            emit stateChanged();
            return;
        }
        // The NetworkManager checkpoint owns the actual rollback deadline;
        // this elapsed timer is only the UI's countdown and revert request.
        m_confirming = true;
        m_seconds = qBound(1, seconds, 120);
        m_elapsed.start();
        m_ticks.start(200);
        emit stateChanged();
    });
    connect(&m_ticks, &QTimer::timeout, this, [this] {
        if (remainingSeconds() == 0 && !m_resolutionPending) {
            m_discardRequested = true;
            m_resolutionPending = true;
            m_backend->revertChange();
        }
        emit stateChanged();
    });
    m_busy = true;
    QTimer::singleShot(0, this, [this] { m_backend->readCurrent(); });
}

int NetworkSettings::remainingSeconds() const
{ return m_confirming ? qMax(0, m_seconds - int(m_elapsed.elapsed() / 1000)) : 0; }

QString NetworkSettings::supportText() const
{
    if (!m_ready) return m_snapshot.reason.isEmpty() ? QStringLiteral("NetworkManager is unavailable.") : m_snapshot.reason;
    if (!m_snapshot.reason.isEmpty()) return m_snapshot.reason;
    if (!connectivityRestrictionReason().isEmpty()) return connectivityRestrictionReason();
    if (!rollbackAvailable()) return QStringLiteral("Rollback checkpoints are unavailable. Connection changes are disabled; inactive saved profiles can still be edited.");
    return m_snapshot.reason;
}

QString NetworkSettings::selectedProfileEditReason() const
{
    const auto *profile = m_snapshot.profile(m_profile);
    if (!profile) return QStringLiteral("Select a saved connection to edit its properties.");
    if (!profile->editable) return profile->reason.isEmpty() ? QStringLiteral("This saved profile cannot be edited here.") : profile->reason;
    if (!connectivityRestrictionReason().isEmpty()) {
        for (const auto &device : m_snapshot.devices)
            if (device.activeProfilePath == profile->path) return connectivityRestrictionReason();
    }
    return {};
}

void NetworkSettings::adopt(const NetworkSnapshot &snapshot, bool preserveStaged)
{
    m_snapshot = snapshot;
    m_ready = snapshot.isValid();
    if (!m_ready) return;
    const auto *profile = snapshot.profile(m_profile);
    if (!profile && preserveStaged) { m_conflict = true; return; }
    if (!profile) {
        if (!snapshot.profiles.isEmpty()) profile = &snapshot.profiles.first();
        m_profile = profile ? profile->path : QString();
    }
    if (!preserveStaged) m_staged = profile ? NetworkTypes::preferences(*profile) : NetworkPreferences{};
    m_dirty = profile && m_staged != NetworkTypes::preferences(*profile);
}

void NetworkSettings::selectProfile(const QString &path)
{
    if (m_busy || !m_ready || path == m_profile) return;
    if (m_dirty) {
        m_error = QStringLiteral("Apply or revert the current profile changes before selecting another profile.");
        emit stateChanged(); return;
    }
    const auto *profile = m_snapshot.profile(path);
    if (!profile) return;
    m_profile = path;
    m_staged = NetworkTypes::preferences(*profile);
    m_error.clear(); m_result.clear();
    emit stateChanged();
}

void NetworkSettings::stagePreferences(const NetworkPreferences &preferences)
{
    if (m_busy || !m_ready || m_conflict) return;
    const auto *profile = m_snapshot.profile(m_profile);
    if (!profile || !profile->editable) return;
    if (!selectedProfileEditReason().isEmpty()) return;
    m_staged = preferences;
    m_dirty = m_staged != NetworkTypes::preferences(*profile);
    m_error.clear(); m_result.clear();
    emit stateChanged();
}

void NetworkSettings::apply()
{
    if (m_busy) return;
    if (!m_ready || m_conflict) {
        m_error = m_conflict ? QStringLiteral("The network configuration changed. Revert to reload it before applying.")
            : QStringLiteral("The current network configuration could not be read.");
        emit applyFinished(false); emit stateChanged(); return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    NetworkRequest request;
    request.action = NetworkRequest::Action::UpdateProfile;
    request.profilePath = m_profile;
    request.preferences = m_staged;
    for (const auto &device : m_snapshot.devices)
        if (device.activeProfilePath == m_profile) { request.devicePath = device.path; break; }
    requestChange(request);
}

void NetworkSettings::requestChange(NetworkRequest request)
{
    if (m_busy || m_scanning) return;
    QString reason;
    const bool editing = request.action == NetworkRequest::Action::UpdateProfile;
    if (!connectivityRestrictionReason().isEmpty()) {
        bool active = false;
        for (const auto &device : m_snapshot.devices)
            active |= device.activeProfilePath == request.profilePath;
        if (!editing || active) {
            m_error = connectivityRestrictionReason();
            request.password.clear();
            emit applyFinished(false); emit stateChanged(); return;
        }
    }
    if (!m_ready || m_conflict || (!editing && m_dirty)
        || !NetworkTypes::validateRequest(m_snapshot, request, reason)) {
        m_error = m_conflict ? QStringLiteral("The network configuration changed. Revert to reload it before applying.")
            : !editing && m_dirty ? QStringLiteral("Apply or revert the staged profile changes before changing connections.")
            : reason.isEmpty() ? QStringLiteral("The current network configuration could not be read.") : reason;
        request.password.clear();
        emit applyFinished(false); emit stateChanged(); return;
    }
    m_request = std::move(request);
    m_discardRequested = false;
    m_baseline = m_snapshot;
    m_phase = Phase::Preflight;
    m_busy = true;
    m_error.clear(); m_result.clear();
    emit stateChanged();
    m_backend->readCurrent();
}

void NetworkSettings::readDone(bool success, const NetworkSnapshot &snapshot, const QString &error)
{
    if (m_phase != Phase::Reading && m_phase != Phase::Preflight) return;
    const bool preflight = m_phase == Phase::Preflight;
    QString reason;
    if (preflight && success && snapshot.isValid() && NetworkTypes::sameSnapshot(m_baseline, snapshot)
        && NetworkTypes::validateRequest(snapshot, m_request, reason)) {
        m_phase = Phase::Preview;
        m_resolutionPending = false;
        // Pass a short-lived copy: a synchronously completing fake/backend can
        // finish and reset the member while beginChange is still on the stack.
        NetworkRequest request = m_request;
        m_backend->beginChange(m_baseline, request);
        request.password.clear();
        m_request.password.clear();
        return;
    }
    const bool changed = m_ready && success && snapshot.isValid() && !NetworkTypes::sameSnapshot(m_snapshot, snapshot);
    const bool dirty = m_dirty;
    if (success && snapshot.isValid()) {
        adopt(snapshot, dirty);
        m_conflict = m_conflict || (changed && dirty) || (preflight && changed);
        if (m_conflict) m_error = QStringLiteral("The network configuration changed. Revert to reload it before applying.");
        else if (preflight) m_error = reason.isEmpty() ? QStringLiteral("The network change could not pass its preflight check.") : reason;
        // Keep an earlier scan or operation error visible until a deliberate
        // user edit or a successful transaction acknowledges it.
    } else {
        m_ready = false;
        m_conflict = dirty;
        m_error = error.isEmpty() ? QStringLiteral("The current network configuration could not be read.") : error;
    }
    m_request.password.clear();
    m_busy = false; m_phase = Phase::Idle;
    m_refreshPending = false;
    if (preflight) emit applyFinished(false);
    emit stateChanged();
}

void NetworkSettings::finish(bool kept, bool restored, const NetworkSnapshot &snapshot, const QString &error)
{
    if (m_phase != Phase::Preview) return;
    const bool verifiedKeep = kept && NetworkTypes::matchesRequest(m_baseline, snapshot, m_request);
    // Profile version counters legitimately increase when rollback restores
    // settings. Compare the actual configuration independently of the counter.
    auto baseline = m_baseline;
    for (auto &profile : baseline.profiles) {
        const auto *current = snapshot.profile(profile.path);
        if (current) profile.versionId = current->versionId;
    }
    for (auto &device : baseline.devices) {
        const auto *current = snapshot.device(device.path);
        if (current && current->activeProfilePath == device.activeProfilePath
            && (device.activeProfilePath.isEmpty() || device.activeProfilePath == QStringLiteral("/") || current->state == 100)) {
            device.activeConnectionPath = current->activeConnectionPath;
            if (NetworkTypes::sameSettings(device.appliedSettings, current->appliedSettings))
                device.appliedVersionId = current->appliedVersionId;
        }
    }
    const bool verifiedRestore = restored && NetworkTypes::sameSnapshot(baseline, snapshot);
    m_ticks.stop(); m_confirming = false; m_resolutionPending = false;
    const bool preserveDraft = m_request.action == NetworkRequest::Action::UpdateProfile
        && !verifiedKeep && !m_discardRequested;
    adopt(snapshot, preserveDraft);
    m_busy = false; m_phase = Phase::Idle;
    m_conflict = !verifiedKeep && !verifiedRestore;
    if (!preserveDraft) m_dirty = false;
    const QString previousError = m_error;
    m_error = verifiedKeep ? QString() : !error.isEmpty() ? error : verifiedRestore ? previousError
        : QStringLiteral("The final network configuration could not be verified. Reload it before making another change.");
    const bool inactiveEdit = m_request.action == NetworkRequest::Action::UpdateProfile && m_request.devicePath.isEmpty();
    m_result = verifiedKeep ? inactiveEdit
        ? QStringLiteral("Saved connection settings were verified; the current connection was not changed.")
        : QStringLiteral("The network change was saved and verified.")
        : verifiedRestore ? QStringLiteral("The previous network configuration was verified. No changes were kept.") : QString();
    m_request.password.clear();
    emit applyFinished(verifiedKeep);
    emit stateChanged();
    if (m_refreshPending) { m_refreshPending = false; refresh(); }
}

void NetworkSettings::keep()
{
    if (!m_confirming || m_resolutionPending) return;
    if (remainingSeconds() <= 0) { revert(); return; }
    m_resolutionPending = true;
    m_backend->keepChange();
    emit stateChanged();
}

void NetworkSettings::revert()
{
    if (m_phase == Phase::Preview) {
        if (!m_resolutionPending) { m_discardRequested = true; m_resolutionPending = true; m_backend->revertChange(); }
        return;
    }
    if (m_busy) return;
    m_dirty = false; m_conflict = false;
    m_error.clear(); m_result.clear();
    refresh();
}

void NetworkSettings::refresh()
{
    if (m_busy || m_scanning) { m_refreshPending = true; return; }
    m_phase = Phase::Reading; m_busy = true;
    emit stateChanged(); m_backend->readCurrent();
}

void NetworkSettings::scan(const QString &devicePath)
{
    if (m_busy || m_scanning || !m_ready) return;
    const auto *device = m_snapshot.device(devicePath);
    if (!device || device->type != 2 || !device->managed) return;
    m_scanning = true;
    m_error.clear(); m_result.clear();
    emit stateChanged(); m_backend->scan(devicePath);
}

void NetworkSettings::activateProfile(const QString &profilePath, const QString &devicePath)
{
    NetworkRequest request;
    request.action = NetworkRequest::Action::ActivateProfile;
    request.profilePath = profilePath; request.devicePath = devicePath;
    requestChange(request);
}

void NetworkSettings::connectWifi(const QString &accessPointPath, const QString &password)
{
    NetworkRequest request;
    request.action = NetworkRequest::Action::ConnectWifi;
    request.accessPointPath = accessPointPath; request.password = password;
    const auto *ap = m_snapshot.accessPoint(accessPointPath);
    if (ap) request.devicePath = ap->devicePath;
    requestChange(std::move(request));
}

void NetworkSettings::disconnectDevice(const QString &devicePath)
{
    NetworkRequest request;
    request.action = NetworkRequest::Action::Disconnect; request.devicePath = devicePath;
    requestChange(request);
}
