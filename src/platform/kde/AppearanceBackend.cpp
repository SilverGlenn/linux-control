#include "AppearanceBackend.h"

#include <KConfig>
#include <KConfigGroup>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QPointer>
#include <functional>
#include <memory>

namespace {
constexpr auto groupName = "Windows";
constexpr auto keyName = "TitlebarDoubleClickCommand";

void observeCompletion(QDBusPendingCallWatcher *pending, QObject *context,
                       std::function<void()> completion)
{
    // Disconnected buses can return an already-completed call before the
    // finished signal is connected. Both delivery paths share one guard.
    auto completed = std::make_shared<bool>(false);
    const QPointer<QDBusPendingCallWatcher> safePending(pending);
    auto once = [completed, safePending, completion = std::move(completion)] {
        if (*completed || !safePending)
            return;
        *completed = true;
        completion();
    };
    QObject::connect(pending, &QDBusPendingCallWatcher::finished, context, once);
    if (pending->isFinished())
        QTimer::singleShot(0, context, once);
}

class KWinReloadService final : public AppearanceReloadService {
public:
    explicit KWinReloadService(const QDBusConnection &connection, QObject *parent = nullptr)
        : AppearanceReloadService(parent), m_bus(connection)
    {
        auto *watcher = new QDBusServiceWatcher(QStringLiteral("org.kde.KWin"),
            m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString &, const QString &, const QString &) { probe(); });
        QTimer::singleShot(0, this, [this] { probe(); });
    }
    bool isAvailable() const override { return m_available; }
    QString unavailableReason() const override { return m_reason; }
    void requestReload(const QString &action) override
    {
        if (!m_available) {
            emit finished(false, m_reason);
            return;
        }
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
            QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("reconfigure"));
        // send() does not await a reply; KWin declares reconfigure Q_NOREPLY.
        if (!m_bus.send(message)) {
            emit finished(false, tr("The setting was saved, but the KWin reload request could not be sent. Revert to reload the saved value."));
            return;
        }
        // v6.6.6 options.h: MaximizeOp=5000, MinimizeOp=5002, NoOp=5018.
        const int expected = action == QLatin1String("Maximize") ? 5000
            : action == QLatin1String("Minimize") ? 5002 : 5018;
        verify(expected, 0, m_generation);
    }
private:
    void probe()
    {
        const quint64 generation = ++m_generation;
        m_available = false;
        m_reason = tr("Checking whether KWin supports reloading window behavior…");
        emit availabilityChanged();
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
            QStringLiteral("/KWin"), QStringLiteral("org.freedesktop.DBus.Introspectable"),
            QStringLiteral("Introspect"));
        auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 2500), this);
        observeCompletion(pending, this,
                [this, pending, generation] {
            const QDBusPendingReply<QString> reply = *pending;
            pending->deleteLater();
            if (generation != m_generation)
                return;
            bool supported = false;
            if (!reply.isError()) {
                QXmlStreamReader xml(reply.value());
                bool kwinInterface = false;
                while (!xml.atEnd()) {
                    xml.readNext();
                    if (xml.isStartElement() && xml.name() == QLatin1String("interface"))
                        kwinInterface = xml.attributes().value(QStringLiteral("name")) == QLatin1String("org.kde.KWin");
                    else if (xml.isEndElement() && xml.name() == QLatin1String("interface"))
                        kwinInterface = false;
                    else if (kwinInterface && xml.isStartElement() && xml.name() == QLatin1String("method")
                             && xml.attributes().value(QStringLiteral("name")) == QLatin1String("reconfigure"))
                        supported = true;
                }
                if (xml.hasError())
                    supported = false;
            }
            if (supported)
                probeReport(generation);
            else {
                m_reason = tr("KWin's window-behavior reload service is unavailable. Apply is disabled.");
                emit availabilityChanged();
            }
        });
    }
    void probeReport(quint64 generation)
    {
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
            QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("supportInformation"));
        auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 1500), this);
        observeCompletion(pending, this, [this, pending, generation] {
            const QDBusPendingReply<QString> reply = *pending;
            pending->deleteLater();
            if (generation != m_generation)
                return;
            int operation = 0;
            m_available = !reply.isError() && parseKWinAppearanceReport(reply.value(), operation);
            m_reason = m_available ? QString()
                : tr("Live window-behavior verification requires KWin 6.6.6's diagnostic interface. Apply is disabled.");
            emit availabilityChanged();
        });
    }
    void verify(int expected, int attempt, quint64 generation)
    {
        // reconfigure schedules a 200ms timer. Poll read-only diagnostics without
        // treating its no-reply method as an acknowledgement or blocking the UI.
        QTimer::singleShot(250, this, [this, expected, attempt, generation] {
            if (generation != m_generation) {
                emit finished(false, tr("The preference was saved, but KWin restarted before its active value could be verified. Revert to reload the saved value."));
                return;
            }
            auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
                QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("supportInformation"));
            auto *pending = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 1000), this);
            observeCompletion(pending, this, [this, pending, expected, attempt, generation] {
                const QDBusPendingReply<QString> reply = *pending;
                pending->deleteLater();
                int operation = 0;
                const bool valid = generation == m_generation && !reply.isError()
                    && parseKWinAppearanceReport(reply.value(), operation);
                if (valid && operation == expected) {
                    emit finished(true, QString());
                } else if (generation == m_generation && attempt < 5) {
                    verify(expected, attempt + 1, generation);
                } else {
                    emit finished(false, tr("The preference was saved, but KWin's active title-bar action could not be verified. Revert to reload the saved value before trying again."));
                }
            });
        });
    }
    QDBusConnection m_bus;
    bool m_available = false;
    quint64 m_generation = 0;
    QString m_reason = tr("Checking KWin's window-behavior reload service…");
};
}

AppearanceReloadService *createKWinAppearanceReloadService(const QDBusConnection &connection, QObject *parent)
{
    return new KWinReloadService(connection, parent);
}

bool parseKWinAppearanceReport(const QString &report, int &operation)
{
    if (report.size() > 65536)
        return false;
    const QStringList lines = report.split(QLatin1Char('\n'));
    if (lines.count(QStringLiteral("KWin version: 6.6.6")) != 1)
        return false;
    const int section = lines.indexOf(QStringLiteral("Options"));
    if (section < 0 || section + 1 >= lines.size() || lines[section + 1] != QLatin1String("======="))
        return false;
    bool found = false;
    const QRegularExpression field(QStringLiteral("^operationTitlebarDblClick: ([0-9]+)$"));
    for (int i = section + 2; i < lines.size() && !lines[i].isEmpty(); ++i) {
        const auto match = field.match(lines[i]);
        if (match.hasMatch()) {
            if (found)
                return false;
            bool ok = false;
            operation = match.captured(1).toInt(&ok);
            if (!ok)
                return false;
            found = true;
        }
    }
    return found;
}

KdeAppearanceBackend::KdeAppearanceBackend(QObject *parent)
    : KdeAppearanceBackend(QStringLiteral("kwinrc"),
                           createKWinAppearanceReloadService(QDBusConnection::sessionBus()), false, parent)
{
}

KdeAppearanceBackend::KdeAppearanceBackend(const QString &absoluteConfigPath,
                                         AppearanceReloadService *reload, QObject *parent)
    : KdeAppearanceBackend(absoluteConfigPath, reload, true, parent)
{
    Q_ASSERT(QFileInfo(absoluteConfigPath).isAbsolute());
}

KdeAppearanceBackend::KdeAppearanceBackend(const QString &configName,
                                         AppearanceReloadService *reload, bool isolated, QObject *parent)
    : AppearanceBackend(parent), m_configName(configName), m_reload(reload), m_isolated(isolated)
{
    Q_ASSERT(reload);
    reload->setParent(this);
    m_userPath = isolated ? configName : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/kwinrc");
    m_watchedFiles << m_userPath;
    if (!isolated) {
        // FullConfig includes global and system defaults/policy. Watch those too.
        for (const auto &dir : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))
            m_watchedFiles << dir + QStringLiteral("/kwinrc") << dir + QStringLiteral("/kdeglobals");
        m_watchedFiles.removeDuplicates();
    }
    auto changed = [this](const QString &) { watchFiles(); m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_debounce, &QTimer::timeout, this, [this] { emit externalChange(); });
    connect(reload, &AppearanceReloadService::availabilityChanged, this, &AppearanceBackend::externalChange);
    connect(reload, &AppearanceReloadService::finished, this, [this](bool success, const QString &error) {
        if (!m_writing)
            return;
        m_writing = false;
        emit writeFinished(success, error);
    });
    watchFiles();
}

void KdeAppearanceBackend::watchFiles()
{
    for (const auto &path : m_watchedFiles) {
        if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
            m_watcher.addPath(path);
        QString directory = QFileInfo(path).absolutePath();
        while (!QDir(directory).exists() && directory != QDir::rootPath())
            directory = QFileInfo(directory).absolutePath();
        if (!m_watcher.directories().contains(directory))
            m_watcher.addPath(directory);
    }
}

bool KdeAppearanceBackend::readState(AppearanceState &state, QString &error) const
{
    // KConfig falls back silently on unreadable files; do not present that as a
    // known authoritative value. Cascaded/global files matter as well.
    for (const auto &path : m_watchedFiles) {
        if (QFileInfo::exists(path)) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                error = tr("Window behavior configuration cannot be read: %1").arg(path);
                return false;
            }
        }
    }
    KConfig config(m_configName, m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
    KConfigGroup group(&config, groupName);
    state.doubleClickAction = group.readEntry(keyName, QStringLiteral("Maximize"));
    state.writable = !config.isImmutable() && !group.isImmutable() && !group.isEntryImmutable(keyName)
        && config.isConfigWritable(false);
    state.reloadAvailable = m_reload->isAvailable();
    if (!state.writable)
        state.unavailableReason = tr("This window-behavior preference is locked by policy or its configuration is not writable.");
    else if (!state.reloadAvailable)
        state.unavailableReason = m_reload->unavailableReason();
    return true;
}

void KdeAppearanceBackend::readCurrent()
{
    AppearanceState state;
    QString error;
    const bool success = readState(state, error);
    emit readFinished(success, state, error);
}

void KdeAppearanceBackend::writeAction(const QString &expected, const QString &action)
{
    if (m_writing)
        return;
    AppearanceState state;
    QString error;
    if (!AppearanceSettings::isSupportedAction(action))
        error = tr("This title-bar action is not supported by Control Panel.");
    else if (!readState(state, error)) { /* Preserve the precise read error. */ }
    else if (state.doubleClickAction != expected)
        error = tr("Window behavior changed before the write. Revert to reload it.");
    else if (!state.canApply())
        error = state.unavailableReason;
    if (!error.isEmpty()) {
        emit writeFinished(false, error);
        return;
    }
    KConfig config(m_configName, m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
    KConfigGroup group(&config, groupName);
    // Recheck policy on the exact config object used to persist the one key.
    if (group.readEntry(keyName, QStringLiteral("Maximize")) != expected) {
        emit writeFinished(false, tr("Window behavior changed before the write. Revert to reload it."));
        return;
    }
    if (config.isImmutable() || group.isImmutable() || group.isEntryImmutable(keyName)) {
        emit writeFinished(false, tr("The title-bar preference is locked by policy."));
        return;
    }
    group.writeEntry(keyName, action);
    if (!config.sync()) {
        config.markAsClean(); // Prevent destructor retry after an observed failure.
        emit writeFinished(false, tr("Window behavior could not be saved. Check configuration permissions."));
        return;
    }
    watchFiles();
    m_writing = true;
    m_reload->requestReload(action);
}
