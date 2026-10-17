#include "WallpaperBackend.h"
#include <KConfig>
#include <KConfigGroup>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
QString jsonLiteral(const QJsonObject &object)
{
    QString result = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    // JSON permits these literal characters; older JavaScript grammars do not.
    return result.replace(QChar(0x2028), QStringLiteral("\\u2028"))
                 .replace(QChar(0x2029), QStringLiteral("\\u2029"));
}

QString descriptorFunction()
{
    return QStringLiteral(
        "function descriptor(d) { d.currentConfigGroup = []; "
        "var lock = immutability(), globalLock = lock === 'mutable' ? 1 : lock === 'userImmutable' ? 2 : lock === 'systemImmutable' ? 4 : 0; "
        "if (globalLock === 0) throw new Error('Unsupported Plasma immutability state.'); "
        "var im = Math.max(Number(d.readConfig('immutability', 1)), globalLock); "
        "d.currentConfigGroup = ['Wallpaper', d.wallpaperPlugin, 'General']; "
        "return {id: Number(d.id), screen: Number(d.screen), plugin: String(d.wallpaperPlugin), "
        "image: String(d.readConfig('Image', '')), fillMode: Number(d.readConfig('FillMode', 2)), immutability: im}; } ");
}

class DbusWallpaperTransport final : public WallpaperScriptTransport {
public:
    explicit DbusWallpaperTransport(const QDBusConnection &connection) : m_connection(connection)
    {
        auto *watcher = new QDBusServiceWatcher(QStringLiteral("org.kde.plasmashell"), connection,
            QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] { emit serviceChanged(); });
    }
    void evaluate(const QString &script) override
    {
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.plasmashell"),
            QStringLiteral("/PlasmaShell"), QStringLiteral("org.kde.PlasmaShell"), QStringLiteral("evaluateScript"));
        message << script;
        auto *watcher = new QDBusPendingCallWatcher(m_connection.asyncCall(message, 5000), this);
        auto done = std::make_shared<bool>(false);
        auto complete = [this, watcher, done] {
            if (*done) return;
            *done = true;
            const QDBusPendingReply<QString> reply = *watcher;
            const bool success = !reply.isError();
            const QString output = success ? reply.value() : QString();
            const QString error = success ? QString() : tr("Plasma background service: %1").arg(reply.error().message());
            watcher->deleteLater(); emit finished(success, output, error);
        };
        connect(watcher, &QDBusPendingCallWatcher::finished, this, complete);
        if (watcher->isFinished()) QTimer::singleShot(0, watcher, complete);
    }
private:
    QDBusConnection m_connection;
};
}

WallpaperScriptTransport *createWallpaperScriptTransport(const QDBusConnection &connection)
{
    return new DbusWallpaperTransport(connection);
}

QString wallpaperReadScript()
{
    return QStringLiteral("(function() { ") + descriptorFunction()
        + QStringLiteral("var ds = desktops(), result = []; for (var i = 0; i < ds.length; ++i) result.push(descriptor(ds[i])); print(JSON.stringify(result)); })();");
}

QString wallpaperWriteScript(const WallpaperDesktop &expected, const QString &image, int fillMode)
{
    QJsonObject baseline{{"id", double(expected.id)}, {"screen", expected.screen}, {"plugin", expected.plugin},
        {"image", expected.image}, {"fillMode", expected.fillMode}, {"immutability", expected.immutability}};
    QJsonObject request{{"image", image}, {"fillMode", fillMode}};
    return QStringLiteral("(function() { ") + descriptorFunction()
        + QStringLiteral("var expected = ") + jsonLiteral(baseline) + QStringLiteral(", request = ") + jsonLiteral(request)
        + QStringLiteral("; var ds = desktops(), selected = null; "
            "for (var i = 0; i < ds.length; ++i) if (Number(ds[i].id) === expected.id) selected = ds[i]; "
            "if (selected === null) throw new Error('Desktop disappeared; no background was changed.'); "
            "var current = descriptor(selected); "
            "for (var key in expected) if (current[key] !== expected[key]) throw new Error('Desktop changed; no background was changed.'); "
            "if (current.screen < 0 || current.immutability === 4) throw new Error('Desktop is unavailable or locked by policy.'); "
            "selected.wallpaperPlugin = 'org.kde.image'; "
            "selected.currentConfigGroup = ['Wallpaper', 'org.kde.image', 'General']; "
            "selected.writeConfig('Image', request.image); selected.writeConfig('FillMode', request.fillMode); "
            "print('background-requested'); })();");
}

bool parseWallpaperState(const QString &json, WallpaperState &state, QString &error)
{
    error.clear();
    auto invalid = [&] { error = QStringLiteral("Plasma returned an unsupported desktop background description."); return false; };
    if (json.toUtf8().size() > 65536) return invalid();
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isArray() || document.array().size() > 64) return invalid();
    WallpaperState parsed; QSet<quint32> ids;
    for (const auto &value : document.array()) {
        if (!value.isObject()) return invalid();
        const auto object = value.toObject();
        auto number = [&](const char *name, double minimum, double maximum) {
            const auto v = object.value(QLatin1String(name)); const double n = v.toDouble(-9999);
            return v.isDouble() && std::isfinite(n) && std::floor(n) == n && n >= minimum && n <= maximum;
        };
        if (!number("id", 1, 4294967295.0) || !number("screen", -1, 255)
            || !number("fillMode", 0, 255) || !number("immutability", 1, 4)
            || !object.value("plugin").isString() || !object.value("image").isString()) return invalid();
        WallpaperDesktop desktop;
        desktop.id = quint32(object.value("id").toDouble()); desktop.screen = object.value("screen").toInt();
        desktop.plugin = object.value("plugin").toString(); desktop.image = object.value("image").toString();
        desktop.fillMode = object.value("fillMode").toInt(); desktop.immutability = object.value("immutability").toInt();
        if (desktop.immutability != 1 && desktop.immutability != 2 && desktop.immutability != 4) return invalid();
        if (ids.contains(desktop.id) || desktop.plugin.isEmpty() || desktop.plugin.size() > 256 || desktop.image.size() > 8192) return invalid();
        ids.insert(desktop.id); parsed.desktops << desktop;
    }
    std::sort(parsed.desktops.begin(), parsed.desktops.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    state = parsed; return true;
}

bool validateWallpaperPicture(const QString &url, QString &error)
{
    const QUrl image(url, QUrl::StrictMode);
    const QFileInfo file(image.toLocalFile());
    if (!image.isValid() || !image.isLocalFile() || !image.host().isEmpty() || image.hasQuery() || image.hasFragment()
        || !file.isAbsolute() || !file.isFile() || !file.isReadable() || file.size() > 64 * 1024 * 1024) {
        error = QStringLiteral("Choose a readable local picture smaller than 64 MiB."); return false;
    }
    QImageReader reader(file.absoluteFilePath());
    const auto size = reader.size();
    if (!reader.canRead() || !size.isValid() || qint64(size.width()) * size.height() > 64000000) {
        error = QStringLiteral("The selected file is not a supported picture, or exceeds 64 million pixels."); return false;
    }
    // Validate decoding before instructing Plasma. Allocation is bounded by the
    // header checks and Qt's image allocation limit (scaled formats may decode smaller).
    reader.setScaledSize(size.scaled(320, 200, Qt::KeepAspectRatio));
    if (reader.read().isNull()) { error = QStringLiteral("The selected picture could not be decoded."); return false; }
    return true;
}

KdeWallpaperBackend::KdeWallpaperBackend(QObject *parent)
    : KdeWallpaperBackend(createWallpaperScriptTransport(QDBusConnection::sessionBus()),
                          QStringLiteral("plasma-org.kde.plasma.desktop-appletsrc"), false, parent) {}

KdeWallpaperBackend::KdeWallpaperBackend(WallpaperScriptTransport *transport, const QString &configFile, QObject *parent)
    : KdeWallpaperBackend(transport, configFile, true, parent) {}

KdeWallpaperBackend::KdeWallpaperBackend(WallpaperScriptTransport *transport, const QString &configName, bool isolated, QObject *parent)
    : WallpaperBackend(parent), m_transport(transport), m_configName(configName), m_isolated(isolated)
{
    Q_ASSERT(transport); transport->setParent(this);
    Q_ASSERT(!isolated || QFileInfo(configName).isAbsolute());
    m_userFile = isolated ? configName : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + '/' + configName;
    connect(transport, &WallpaperScriptTransport::finished, this, [this](bool success, const QString &output, const QString &error) {
        const Phase phase = m_phase; m_phase = Phase::Idle;
        if (phase == Phase::Read) {
            WallpaperState state; QString parseError;
            const bool valid = success && parseWallpaperState(output, state, parseError);
            if (valid) applyCapabilities(state);
            emit readFinished(valid, state, success ? parseError : error);
        } else if (phase == Phase::Write) emit writeFinished(success && output.trimmed() == QStringLiteral("background-requested"),
            success && output.trimmed() != QStringLiteral("background-requested") ? tr("Plasma did not acknowledge the background request.") : error);
    });
    connect(transport, &WallpaperScriptTransport::serviceChanged, this, &WallpaperBackend::externalChange);
    m_debounce.setSingleShot(true); m_debounce.setInterval(150);
    connect(&m_debounce, &QTimer::timeout, this, &WallpaperBackend::externalChange);
    auto changed = [this] { watchFiles(); m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    watchFiles();
    if (!isolated) {
        m_poll.setInterval(10000);
        connect(&m_poll, &QTimer::timeout, this, [this] { if (m_phase == Phase::Idle) emit externalChange(); });
        m_poll.start();
    }
}

void KdeWallpaperBackend::watchFiles()
{
    QStringList paths{m_userFile, QFileInfo(m_userFile).absolutePath()};
    if (!m_isolated) for (const auto &base : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) {
        paths << base << base + '/' + m_configName;
    }
    for (const auto &path : paths) if (QFileInfo::exists(path)
        && !m_watcher.files().contains(path) && !m_watcher.directories().contains(path)) m_watcher.addPath(path);
}

void KdeWallpaperBackend::applyCapabilities(WallpaperState &state) const
{
    KConfig config(m_configName, m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
    const auto containments = config.group(QStringLiteral("Containments"));
    for (auto &desktop : state.desktops) {
        const auto group = containments.group(QString::number(desktop.id));
        const auto image = group.group(QStringLiteral("Wallpaper")).group(QStringLiteral("org.kde.image")).group(QStringLiteral("General"));
        desktop.writable = desktop.immutability != 4 && !config.isImmutable() && config.isConfigWritable(false) && !group.isImmutable()
            && !group.isEntryImmutable("wallpaperplugin") && !image.isImmutable()
            && !image.isEntryImmutable("Image") && !image.isEntryImmutable("FillMode");
        if (!desktop.writable) desktop.reason = tr("This desktop background is locked by policy or its configuration is not writable.");
        else if (desktop.screen < 0) { desktop.writable = false; desktop.reason = tr("This desktop is not currently displayed. Activate its activity or connect its screen first."); }
    }
}

void KdeWallpaperBackend::readCurrent()
{
    if (m_phase != Phase::Idle) return;
    m_phase = Phase::Read; m_transport->evaluate(wallpaperReadScript());
}

void KdeWallpaperBackend::writePicture(const WallpaperDesktop &expected, const QString &image, int fillMode)
{
    if (m_phase != Phase::Idle) return;
    QString error;
    WallpaperState current; current.desktops << expected; applyCapabilities(current);
    if (!current.desktops.first().writable || !expected.writable || expected.screen < 0
        || !WallpaperSettings::supportedFillMode(fillMode) || !validateWallpaperPicture(image, error)) {
        if (error.isEmpty()) error = tr("This desktop background is unavailable or locked by KDE policy.");
        emit writeFinished(false, error); return;
    }
    m_phase = Phase::Write; m_transport->evaluate(wallpaperWriteScript(expected, image, fillMode));
}
