#include "FontBackend.h"
#include <KConfig>
#include <KConfigGroup>
#include <KConfigGui>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <fontconfig/fontconfig.h>
#include <memory>

namespace {
constexpr const char *fontKeys[] = {"font", "fixed", "smallestReadableFont", "toolBarFont", "menuFont", "activeFont"};
constexpr const char *renderKeys[] = {"XftAntialias", "XftHintStyle", "XftSubPixel"};
constexpr qint64 maximumConfigBytes = 1024 * 1024;
QStringList fontconfigFiles()
{
    QStringList files;
    // Load current includes anew so an external edit to a parent/include is part
    // of preflight. This parses configuration; it does not build font caches.
    FcConfig *config = FcInitLoadConfig();
    if (!config) return files;
    FcStrList *list = FcConfigGetConfigFiles(config);
    if (list) {
        while (const FcChar8 *file = FcStrListNext(list))
            files << QString::fromLocal8Bit(reinterpret_cast<const char *>(file));
        FcStrListDone(list);
    }
    FcConfigDestroy(config);
    return files;
}
QString fontconfigPath()
{
    // KXftConfig::getConfigFile(), Plasma 6.6.6: prefer a loaded user
    // fonts.conf/.fonts.conf, then the first loaded user file, then XDG path.
    QStringList local;
    const QString home = QDir::homePath() + QLatin1Char('/');
    for (const auto &path : fontconfigFiles())
        if (path.startsWith(home) && QFileInfo(path).isFile()) local << path;
    for (const auto &path : local)
        if (path.endsWith(QLatin1String("/fonts.conf")) || path.endsWith(QLatin1String("/.fonts.conf"))) return path;
    if (!local.isEmpty()) return local.first();
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/fontconfig/fonts.conf");
}
bool readableBytes(const QString &path, QByteArray &bytes, QString &error)
{
    bytes.clear();
    if (!QFileInfo::exists(path)) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumConfigBytes) {
        error = QObject::tr("Font settings cannot be read safely: %1").arg(path); return false;
    }
    bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        error = QObject::tr("Font settings could not be read: %1").arg(path); return false;
    }
    return true;
}
bool loadXml(const QString &path, QDomDocument &doc, QString &error)
{
    QByteArray bytes;
    if (!readableBytes(path, bytes, error)) return false;
    if (bytes.isEmpty() && !QFileInfo::exists(path)) {
        doc.appendChild(doc.createProcessingInstruction(QStringLiteral("xml"), QStringLiteral("version=\"1.0\"")));
        doc.appendChild(doc.createElement(QStringLiteral("fontconfig")));
        return true;
    }
    // No custom entities: expansion can make a small input unbounded, and the
    // backend only needs ordinary fontconfig XML, not a general XML processor.
    if (bytes.contains("<!ENTITY")) {
        error = QObject::tr("Custom XML entities in fontconfig are unsupported; no settings were changed."); return false;
    }
    auto parsed = doc.setContent(bytes, QDomDocument::ParseOption::PreserveSpacingOnlyNodes);
    if (!parsed || doc.documentElement().tagName() != QLatin1String("fontconfig")) {
        error = QObject::tr("The fontconfig file is invalid; no settings were changed: %1").arg(path); return false;
    }
    return true;
}
// Only unconditional single-edit font-target rules are editable. Pattern-target
// rendering uses a different fontconfig matching stage and is refused explicitly.
// Conditional rules,
// aliases, includes, exclusion ranges, comments and unrelated edits stay intact.
QString canonicalProperty(const QDomElement &match, QString *value = nullptr)
{
    if (match.tagName() != QLatin1String("match") || match.attributes().count() != 1
        || match.attribute(QStringLiteral("target")) != QLatin1String("font")) return {};
    auto edit = match.firstChildElement();
    if (edit.isNull() || !edit.nextSiblingElement().isNull() || edit.tagName() != QLatin1String("edit")
        || edit.attributes().count() != 2 || edit.attribute(QStringLiteral("mode")) != QLatin1String("assign")) return {};
    const QString name = edit.attribute(QStringLiteral("name"));
    if (name != QLatin1String("antialias") && name != QLatin1String("hintstyle")
        && name != QLatin1String("rgba") && name != QLatin1String("hinting")) return {};
    auto scalar = edit.firstChildElement();
    const QString tag = (name == QLatin1String("antialias") || name == QLatin1String("hinting"))
        ? QStringLiteral("bool") : QStringLiteral("const");
    if (scalar.tagName() != tag || !scalar.nextSiblingElement().isNull()
        || scalar.attributes().count() || !scalar.firstChildElement().isNull()) return {};
    if (value) *value = scalar.text().trimmed().toLower();
    return name;
}
bool renderingFromXml(const QDomDocument &doc, FontPreferences &p, QString &error)
{
    QSet<QString> properties;
    for (auto node = doc.documentElement().firstChildElement(); !node.isNull(); node = node.nextSiblingElement()) {
        QString value;
        const auto name = canonicalProperty(node, &value);
        if (!name.isEmpty()) {
            // Keeping earlier unconditional assignments while changing the last
            // one can alter intervening conditional rules; removing all of them
            // is not a safe representation of one staged override either.
            if (properties.contains(name)) {
                error = QObject::tr("This fontconfig file has duplicate unconditional rendering rules for %1. Use KDE System Settings to reconcile them before editing here.").arg(name);
                return false;
            }
            properties.insert(name);
        }
        if (node.tagName() == QLatin1String("match") && name.isEmpty()
            && node.firstChildElement(QStringLiteral("test")).isNull()) {
            for (auto edit = node.firstChildElement(QStringLiteral("edit")); !edit.isNull(); edit = edit.nextSiblingElement(QStringLiteral("edit"))) {
                const QString property = edit.attribute(QStringLiteral("name"));
                if (property == QLatin1String("antialias") || property == QLatin1String("rgba")
                    || property == QLatin1String("hintstyle") || property == QLatin1String("hinting")) {
                    error = node.attribute(QStringLiteral("target")) == QLatin1String("pattern")
                        ? QObject::tr("This fontconfig file has pattern-target rendering rules. Their matching stage cannot be represented safely by these font-target controls. Use KDE System Settings to reconcile them.")
                        : QObject::tr("This fontconfig file has unconditional rendering rules that cannot be edited safely here. Use KDE System Settings to reconcile them.");
                    return false;
                }
            }
        }
        if (name == QLatin1String("antialias")) p.antialias = value;
        if (name == QLatin1String("hintstyle")) p.hinting = value;
        if (name == QLatin1String("rgba")) p.subpixel = value;
        // Standalone hinting can disable a selected hint style. Refuse an
        // inconsistent combination rather than display a fabricated value.
    }
    QString hinting;
    bool hasHinting = false;
    for (auto node = doc.documentElement().firstChildElement(); !node.isNull(); node = node.nextSiblingElement()) {
        QString value;
        if (canonicalProperty(node, &value) == QLatin1String("hinting")) {
            hinting = value;
            hasHinting = true;
        }
    }
    if (hasHinting && ((hinting != QLatin1String("true") && hinting != QLatin1String("false"))
        || p.hinting.isEmpty() || ((hinting == QLatin1String("false")) != (p.hinting == QLatin1String("hintnone"))))) {
        error = QObject::tr("The fontconfig hinting rules do not match a supported KDE hint style. Use KDE System Settings to reconcile them.");
        return false;
    }
    if (!FontSettings::isSupported(p)) {
        error = QObject::tr("The saved fonts or rendering rules use unsupported values. No settings were changed."); return false;
    }
    return true;
}
void updateRule(QDomDocument &doc, const QString &name, const QString &value)
{
    QDomNode last;
    auto root = doc.documentElement();
    for (auto node = root.firstChildElement(); !node.isNull();) {
        const auto next = node.nextSiblingElement();
        if (canonicalProperty(node) == name) {
            if (value.isEmpty()) root.removeChild(node);
            else last = node;
        }
        node = next;
    }
    if (value.isEmpty()) return;
    auto match = doc.createElement(QStringLiteral("match"));
    match.setAttribute(QStringLiteral("target"), QStringLiteral("font"));
    auto edit = doc.createElement(QStringLiteral("edit"));
    edit.setAttribute(QStringLiteral("name"), name);
    edit.setAttribute(QStringLiteral("mode"), QStringLiteral("assign"));
    auto scalar = doc.createElement(name == QLatin1String("antialias") || name == QLatin1String("hinting")
        ? QStringLiteral("bool") : QStringLiteral("const"));
    scalar.appendChild(doc.createTextNode(value)); edit.appendChild(scalar); match.appendChild(edit);
    if (last.isNull()) {
        // Keep later conditional exclusions and per-application rules effective.
        // An explicit general choice must not silently override those rules.
        QDomNode before;
        for (auto node = root.firstChildElement(); !node.isNull(); node = node.nextSiblingElement()) {
            if (node.tagName() != QLatin1String("match") || node.firstChildElement(QStringLiteral("test")).isNull()) continue;
            for (auto child = node.firstChildElement(QStringLiteral("edit")); !child.isNull(); child = child.nextSiblingElement(QStringLiteral("edit")))
                if (child.attribute(QStringLiteral("name")) == name) { before = node; break; }
            if (!before.isNull()) break;
        }
        if (before.isNull()) root.appendChild(match); else root.insertBefore(match, before);
    } else root.replaceChild(match, last);
}
bool pathWritable(const QString &path)
{
    if (QFileInfo::exists(path)) return QFileInfo(path).isFile() && QFileInfo(path).isWritable();
    QString ancestor = QFileInfo(path).absolutePath();
    while (!QFileInfo::exists(ancestor) && ancestor != QDir::rootPath()) ancestor = QFileInfo(ancestor).absolutePath();
    return QFileInfo(ancestor).isDir() && QFileInfo(ancestor).isWritable();
}
class KdeFontReload final : public FontReloadService {
public:
    KdeFontReload(const QDBusConnection &bus, QObject *parent) : FontReloadService(parent), m_bus(bus) {}
    bool isAvailable() const override
    { return m_bus.isConnected() && QGuiApplication::platformName() == QLatin1String("wayland") && qEnvironmentVariableIsSet("KDE_FULL_SESSION"); }
    QString unavailableReason() const override
    { return tr("Font Apply requires a KDE Wayland session and its session bus. X11 resource export is not supported here."); }
    void requestReload() override
    {
        if (!isAvailable()) { emit finished(false, unavailableReason()); return; }
        const bool sent = m_bus.send(QDBusMessage::createSignal(QStringLiteral("/KDEPlatformTheme"),
            QStringLiteral("org.kde.KDEPlatformTheme"), QStringLiteral("refreshFonts")));
        emit finished(sent, sent ? QString() : tr("Font settings were saved, but KDE's refresh request could not be sent. Restart applications or sign out and back in."));
    }
private:
    QDBusConnection m_bus;
};
}

FontReloadService *createKdeFontReloadService(const QDBusConnection &bus, QObject *parent)
{ return new KdeFontReload(bus, parent); }

bool fontConfigurationRevision(const QStringList &paths, QByteArray &revision, QString &error)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &path : paths) {
        hash.addData(path.toUtf8()); hash.addData(QByteArray(1, '\0'));
        if (QFileInfo(path).isDir()) {
            const QDir directory(path);
            if (!directory.isReadable()) { error = QObject::tr("The fontconfig include directory cannot be read: %1").arg(path); return false; }
            hash.addData(QByteArray("directory"));
            for (const auto &name : directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name)) {
                hash.addData(name.toUtf8()); hash.addData(QByteArray(1, '\0'));
            }
        } else {
            QByteArray bytes;
            if (!readableBytes(path, bytes, error)) return false;
            hash.addData(QFileInfo::exists(path) ? QByteArray("present") : QByteArray("absent")); hash.addData(bytes);
        }
        hash.addData(QByteArray(1, '\0'));
    }
    revision = hash.result(); return true;
}

KdeFontBackend::KdeFontBackend(QObject *parent)
    : KdeFontBackend(QStringLiteral("kdeglobals"), fontconfigPath(), createKdeFontReloadService(QDBusConnection::sessionBus()), false, parent) {}
KdeFontBackend::KdeFontBackend(const QString &config, const QString &xml, FontReloadService *reload, QObject *parent)
    : KdeFontBackend(config, xml, reload, true, parent)
{ Q_ASSERT(QFileInfo(config).isAbsolute() && QFileInfo(xml).isAbsolute()); }
KdeFontBackend::KdeFontBackend(const QString &config, const QString &xml, FontReloadService *reload, bool isolated, QObject *parent)
    : FontBackend(parent), m_configName(config), m_userPath(isolated ? config : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kdeglobals")),
      m_fontconfigPath(xml), m_reload(reload), m_isolated(isolated)
{
    Q_ASSERT(reload); reload->setParent(this);
    m_debounce.setSingleShot(true); m_debounce.setInterval(80);
    m_reloadTimeout.setSingleShot(true); m_reloadTimeout.setInterval(3000);
    connect(&m_reloadTimeout, &QTimer::timeout, this, [this] {
        if (!m_writing) return;
        m_writing = false;
        emit writeFinished(false, tr("Font settings were saved, but the KDE refresh request timed out. Revert to read the saved settings."));
    });
    connect(reload, &FontReloadService::finished, this, [this](bool ok, const QString &error) {
        if (!m_writing) return;
        m_reloadTimeout.stop(); m_writing = false; emit writeFinished(ok, error);
    });
    connect(reload, &FontReloadService::availabilityChanged, this, &FontBackend::externalChange);
    auto changed = [this](const QString &) { watchFiles(); m_debounce.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    connect(&m_debounce, &QTimer::timeout, this, &FontBackend::externalChange);
    watchFiles();
}
QStringList KdeFontBackend::relevantFiles() const
{
    QStringList paths{m_userPath, m_fontconfigPath};
    if (!m_isolated) {
        for (const auto &dir : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) paths << dir + QStringLiteral("/kdeglobals");
        paths << fontconfigFiles();
    }
    paths.removeDuplicates(); paths.sort(); return paths;
}
void KdeFontBackend::watchFiles()
{
    for (const auto &path : relevantFiles()) {
        if (QFileInfo::exists(path) && !m_watcher.files().contains(path) && !m_watcher.directories().contains(path)) m_watcher.addPath(path);
        QString dir = QFileInfo(path).absolutePath();
        while (!QDir(dir).exists() && dir != QDir::rootPath()) dir = QFileInfo(dir).absolutePath();
        if (!m_watcher.directories().contains(dir)) m_watcher.addPath(dir);
    }
}
bool KdeFontBackend::readState(FontState &state, QString &error) const
{
    if (!QFileInfo(m_fontconfigPath).isAbsolute() || (m_isolated && !QFileInfo(m_configName).isAbsolute())) {
        error = tr("Font settings require absolute configuration paths."); return false;
    }
    if (!m_isolated && fontconfigPath() != m_fontconfigPath) {
        error = tr("KDE's active user fontconfig file changed. Reopen Control Panel before applying font settings."); return false;
    }
    if (!fontConfigurationRevision(relevantFiles(), state.revision, error)) return false;
    KConfig config(m_configName, m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
    state.writable = !config.isImmutable() && config.isConfigWritable(false) && pathWritable(m_fontconfigPath);
    for (int role = 0; role < 6; ++role) {
        KConfigGroup group(&config, role == 5 ? "WM" : "General");
        QFont fallback(role == 1 ? QStringLiteral("Hack") : QStringLiteral("Noto Sans"), role == 2 ? 8 : 10);
        fallback.setStyleName(QStringLiteral("Regular"));
        if (group.hasKey(fontKeys[role])) {
            QFont parsed;
            if (!parsed.fromString(group.readEntry(fontKeys[role], QString()))) {
                error = tr("A saved KDE font is invalid: %1. No settings were changed.").arg(QString::fromLatin1(fontKeys[role])); return false;
            }
        }
        state.preferences.fonts[role] = group.readEntry(fontKeys[role], fallback);
        state.writable &= !group.isImmutable() && !group.isEntryImmutable(fontKeys[role]);
    }
    KConfigGroup general(&config, "General");
    for (auto key : renderKeys) state.writable &= !general.isImmutable() && !general.isEntryImmutable(key);
    QDomDocument doc;
    if (!loadXml(m_fontconfigPath, doc, error) || !renderingFromXml(doc, state.preferences, error)) return false;
    state.reloadAvailable = m_reload->isAvailable();
    if (!state.writable) state.unavailableReason = tr("Font settings are locked by policy or their configuration files are not writable.");
    else if (!state.reloadAvailable) state.unavailableReason = m_reload->unavailableReason();
    return true;
}
void KdeFontBackend::readCurrent()
{
    FontState state; QString error;
    const bool ok = readState(state, error);
    watchFiles(); emit readFinished(ok, state, error);
}
void KdeFontBackend::writePreferences(const FontState &expected, const FontPreferences &requested)
{
    if (m_writing) { emit writeFinished(false, tr("A font refresh is already in progress.")); return; }
    FontState current; QString error;
    if (!FontSettings::isSupported(requested)) {
        emit writeFinished(false, tr("The selected fonts or rendering settings are unsupported. No settings were changed.")); return;
    }
    if (!readState(current, error) || !current.canApply()
        || current.preferences != expected.preferences || current.revision != expected.revision) {
        emit writeFinished(false, !error.isEmpty() ? error : !current.canApply() ? current.unavailableReason
            : tr("Font settings changed before the write. Revert to reload them; no settings were changed.")); return;
    }
    QDomDocument doc;
    if (!loadXml(m_fontconfigPath, doc, error)) { emit writeFinished(false, error); return; }
    const bool renderingChanged = current.preferences.antialias != requested.antialias
        || current.preferences.hinting != requested.hinting || current.preferences.subpixel != requested.subpixel;
    if (current.preferences.antialias != requested.antialias) updateRule(doc, QStringLiteral("antialias"), requested.antialias);
    if (current.preferences.subpixel != requested.subpixel) updateRule(doc, QStringLiteral("rgba"), requested.subpixel);
    if (current.preferences.hinting != requested.hinting) {
        updateRule(doc, QStringLiteral("hintstyle"), requested.hinting);
        updateRule(doc, QStringLiteral("hinting"), requested.hinting.isEmpty() ? QString()
            : requested.hinting == QLatin1String("hintnone") ? QStringLiteral("false") : QStringLiteral("true"));
    }
    // Two files cannot be committed atomically together. Report any partial save
    // and read it back through the session instead of claiming rollback/success.
    if (renderingChanged) {
        if (!QDir().mkpath(QFileInfo(m_fontconfigPath).absolutePath())) {
            emit writeFinished(false, tr("The fontconfig directory could not be created. No settings were changed.")); return;
        }
        QSaveFile file(m_fontconfigPath);
        const QByteArray xml = doc.toByteArray(-1);
        if (!file.open(QIODevice::WriteOnly) || file.write(xml) != xml.size() || !file.commit()) {
            emit writeFinished(false, tr("The fontconfig rendering preferences could not be saved. KDE font preferences were not written.")); return;
        }
    }
    KConfig config(m_configName, m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
    bool stillWritable = !config.isImmutable() && config.isConfigWritable(false);
    for (int role = 0; role < 6; ++role) {
        KConfigGroup group(&config, role == 5 ? "WM" : "General");
        stillWritable &= !group.isImmutable() && !group.isEntryImmutable(fontKeys[role])
            && group.readEntry(fontKeys[role], current.preferences.fonts[role]) == current.preferences.fonts[role];
    }
    KConfigGroup policyGroup(&config, "General");
    for (auto key : renderKeys) stillWritable &= !policyGroup.isImmutable() && !policyGroup.isEntryImmutable(key);
    if (!stillWritable) {
        emit writeFinished(false, renderingChanged
            ? tr("Fontconfig rendering preferences were saved, but KDE font preferences changed or became locked before synchronization. Revert to read the partial save.")
            : tr("KDE font preferences changed or became locked before the write. No font preferences were written.")); return;
    }
    for (int role = 0; role < 6; ++role)
        if (current.preferences.fonts[role] != requested.fonts[role])
            KConfigGroup(&config, role == 5 ? "WM" : "General").writeEntry(fontKeys[role], requested.fonts[role]);
    KConfigGroup general(&config, "General");
    auto mirror = [&general](const char *key, const QString &value, bool boolean) {
        if (value.isEmpty()) general.revertToDefault(key);
        else if (boolean) general.writeEntry(key, value == QLatin1String("true"));
        else general.writeEntry(key, value);
    };
    if (current.preferences.antialias != requested.antialias) mirror("XftAntialias", requested.antialias, true);
    if (current.preferences.hinting != requested.hinting) mirror("XftHintStyle", requested.hinting, false);
    if (current.preferences.subpixel != requested.subpixel) mirror("XftSubPixel", requested.subpixel, false);
    if (!(m_isolated && m_syncForTest ? m_syncForTest(config) : config.sync())) {
        // A failed sync leaves dirty entries that KConfig otherwise retries on
        // destruction, possibly after the failure observer restores writability.
        config.markAsClean();
        emit writeFinished(false, renderingChanged
            ? tr("Fontconfig rendering preferences were saved, but KDE font preferences could not be synchronized. Revert to read the partial save.")
            : tr("KDE font preferences could not be synchronized. Revert to read the saved settings.")); return;
    }
    FontState saved;
    KConfig checkConfig(m_configName, m_isolated ? KConfig::SimpleConfig : KConfig::FullConfig);
    KConfigGroup checkGeneral(&checkConfig, "General");
    bool mirrorsMatch = true;
    if (current.preferences.antialias != requested.antialias && !requested.antialias.isEmpty())
        mirrorsMatch &= checkGeneral.hasKey("XftAntialias") && checkGeneral.readEntry("XftAntialias", requested.antialias != QLatin1String("true")) == (requested.antialias == QLatin1String("true"));
    if (current.preferences.hinting != requested.hinting && !requested.hinting.isEmpty())
        mirrorsMatch &= checkGeneral.readEntry("XftHintStyle", QString()) == requested.hinting;
    if (current.preferences.subpixel != requested.subpixel && !requested.subpixel.isEmpty())
        mirrorsMatch &= checkGeneral.readEntry("XftSubPixel", QString()) == requested.subpixel;
    if (!readState(saved, error) || saved.preferences != requested || !mirrorsMatch) {
        emit writeFinished(false, tr("Font settings were written, but their saved values could not be verified. Revert to read the partial save. %1").arg(error)); return;
    }
    watchFiles(); m_writing = true; m_reloadTimeout.start(); m_reload->requestReload();
}
