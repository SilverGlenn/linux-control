#include "ScreenLockAppearance.h"
#include "settings/AeroThemeContract.h"
#include <KConfig>
#include <KConfigGroup>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace ScreenLockAppearance {
namespace {
const QString Shell = QStringLiteral("io.gitgud.wackyideas.desktop");
const QString LoginTheme = QStringLiteral("sddm-theme-mod");
bool readableFile(const QString &path, qint64 limit = 2 * 1024 * 1024)
{
    const QFileInfo info(path);
    return info.isAbsolute() && info.isFile() && info.isReadable() && info.size() <= limit;
}
QByteArray bytes(const QString &path)
{
    if (!readableFile(path)) return {};
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}
Evidence inspect(const Sources &s)
{
    Evidence out; out.shellId = s.shellId; out.configPath = s.sddmConfig; out.providerPath = s.providerPath;
    out.reason = QStringLiteral("Aero's lock background shares privileged SDDM assets. Changing it requires a separately audited authorization and asset transaction; this page cannot write it. KDE lock-wallpaper settings do not control this Aero screen.");
    const auto required = AeroThemeContract::identities();
    if (s.themeIdentities.value(QStringLiteral("globalTheme")) != required.value(QStringLiteral("globalTheme")) || s.shellId != Shell)
        out.issues << QStringLiteral("The selected theme or shell does not match the fixed approved Aero identity.");
    const auto metadata = QJsonDocument::fromJson(bytes(QDir(s.shellDirectory).filePath(QStringLiteral("metadata.json")))).object();
    if (metadata.value(QStringLiteral("KPlugin")).toObject().value(QStringLiteral("Id")).toString() != Shell
        || metadata.value(QStringLiteral("X-Plasma-APIVersion")).toVariant().toInt() < 2)
        out.issues << QStringLiteral("The Aero lock-screen package identity or greeter API is unavailable.");
    const QString auth = QDir(s.shellDirectory).filePath(QStringLiteral("contents/lockscreen/AuthUI.qml"));
    if (!readableFile(auth, 64 * 1024 * 1024) || s.authUiDigest.isEmpty() || AeroThemeContract::inspectAssetDigest(auth) != s.authUiDigest.toLatin1())
        out.issues << QStringLiteral("The audited Aero authentication UI bytes are missing or changed.");
    const QString lock = QDir(s.shellDirectory).filePath(QStringLiteral("contents/lockscreen/LockScreen.qml"));
    if (!readableFile(lock, 64 * 1024 * 1024) || s.lockScreenDigest.isEmpty() || AeroThemeContract::inspectAssetDigest(lock) != s.lockScreenDigest.toLatin1())
        out.issues << QStringLiteral("The audited Aero lock-screen entry bytes are missing or changed.");
    out.approvedIdentity = out.issues.isEmpty();
    if (!readableFile(s.providerPath, 64 * 1024 * 1024) || s.providerDigest.isEmpty()
        || AeroThemeContract::inspectAssetDigest(s.providerPath) != s.providerDigest.toLatin1())
        out.issues << QStringLiteral("The audited Aero SDDM reader bytes are missing or changed; its current behavior is unverified.");
    out.providerVerified = out.approvedIdentity && out.issues.isEmpty();
    if (!QFileInfo(s.sddmConfig).isAbsolute() || (QFileInfo::exists(s.sddmConfig) && !readableFile(s.sddmConfig)))
        out.issues << QStringLiteral("The SDDM configuration is unreadable or exceeds the evidence size limit.");
    else {
        // Matches the audited custom provider: one SimpleConfig file, no global cascade.
        KConfig config(s.sddmConfig, KConfig::SimpleConfig);
        out.loginTheme = config.group(QStringLiteral("Theme")).readEntry("Current", LoginTheme);
    }
    if (out.loginTheme != LoginTheme) {
        out.issues << QStringLiteral("The SDDM theme is outside the fixed approved Aero background contract.");
    } else if (out.providerVerified) {
        QString selected;
        // Native reader uses locateAll(GenericDataLocation,"sddm").last().
        for (const auto &root : s.dataRoots) {
            const QString candidate = QDir(root).filePath(QStringLiteral("sddm"));
            if (QFileInfo(candidate).isDir()) selected = candidate;
        }
        if (selected.isEmpty() || selected.startsWith(QStringLiteral("/home")) || selected.startsWith(QStringLiteral("~/"))) {
            out.issues << QStringLiteral("The native SDDM data resolution is unavailable or rejected by its own home-directory guard.");
        } else {
            const QString theme = QDir(selected).filePath(QStringLiteral("themes/") + LoginTheme);
            for (const auto &entry : {QStringLiteral("background"), QStringLiteral("default-background")}) {
                const QString path = QDir(theme).filePath(entry);
                if (QFileInfo::exists(path)) { out.backgroundPath = path; break; }
            }
            if (out.backgroundPath.isEmpty()) {
                // The provider falls back to theme.conf.user with theme.conf as an extra source.
                const QString user = QDir(theme).filePath(QStringLiteral("theme.conf.user"));
                const QString base = QDir(theme).filePath(QStringLiteral("theme.conf"));
                if ((QFileInfo::exists(user) && !readableFile(user)) || (QFileInfo::exists(base) && !readableFile(base))) {
                    out.issues << QStringLiteral("The fallback theme configuration cannot be inspected safely.");
                } else {
                    KConfig config(user, s.includeGlobalFallback ? KConfig::FullConfig : KConfig::NoGlobals); config.addConfigSources({base});
                    const QString relative = config.group(QStringLiteral("General")).readEntry("background", QString());
                    out.backgroundPath = QDir::isAbsolutePath(relative) ? relative : QDir(theme).filePath(relative);
                }
            }
            if (readableFile(out.backgroundPath, 64 * 1024 * 1024)) {
                out.backgroundDigest = AeroThemeContract::inspectAssetDigest(out.backgroundPath);
                out.backgroundAvailable = !out.backgroundDigest.isEmpty();
            }
            if (!out.backgroundAvailable) out.issues << QStringLiteral("The selected background is missing, unreadable, or outside the bounded file evidence limit.");
        }
    }
    out.description = out.backgroundAvailable
        ? QStringLiteral("Aero background file: %1. This is passive file evidence, not a verified image on an active lock screen. The native reader watches SDDM configuration, not this asset; live reload is unverified.").arg(out.backgroundPath)
        : QStringLiteral("Aero lock-screen background evidence is unavailable. No wallpaper plugin or lock-screen process was loaded.");
    return out;
}
Evidence readCurrent()
{
    Sources s; s.themeIdentities = AeroThemeContract::identities();
    KConfig themeConfig(QStringLiteral("kdeglobals"), KConfig::FullConfig);
    s.themeIdentities[QStringLiteral("globalTheme")] = themeConfig.group(QStringLiteral("KDE")).readEntry("LookAndFeelPackage", QString());
    KConfig shellConfig(QStringLiteral("plasmashellrc"), KConfig::FullConfig);
    const QString fallback = qEnvironmentVariable("PLASMA_DEFAULT_SHELL", QStringLiteral("org.kde.plasma.desktop"));
    s.shellId = shellConfig.group(QStringLiteral("Shell")).readEntry("ShellPackage", fallback);
    if (s.shellId.isEmpty()) s.shellId = fallback;
    s.shellDirectory = QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("plasma/shells/") + Shell, QStandardPaths::LocateDirectory);
    s.authUiDigest = QStringLiteral("fc37b26c1aba9f8d22da573a8b5389bf680dbd9083b5cd1917f2070264a66e14");
    s.lockScreenDigest = QStringLiteral("cd4291f373caa26f91cea88d1fc8f8f23bd53c2f919bae45cb12b29455dbd8ed");
    s.providerPath = QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/qml/aeroshell/utils/libaeroshellutilsplugin.so");
    s.providerDigest = QStringLiteral("0c6cb27868f7aea8b62a5ebe0920343b506525a0c3c92e4047636424cdcb16f4");
    s.sddmConfig = QStringLiteral("/etc/sddm.conf.d/kde_settings.conf");
    s.dataRoots = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    return inspect(s);
}
}
