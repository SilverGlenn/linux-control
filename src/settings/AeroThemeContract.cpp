#include "AeroThemeContract.h"
#include "platform/kde/AeroEffectBackend.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLibrary>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <filesystem>

namespace {
QVariantMap aeroPalette()
{
    return {
        {"ColorEffects:Disabled", QVariantMap{{"Color", "56,56,56"}, {"ColorAmount", "0"}, {"ColorEffect", "0"}, {"ContrastAmount", "0.65"}, {"ContrastEffect", "1"}, {"IntensityAmount", "0.1"}, {"IntensityEffect", "2"}}},
        {"ColorEffects:Inactive", QVariantMap{{"ChangeSelectionColor", "true"}, {"Color", "112,111,110"}, {"ColorAmount", "0.025"}, {"ColorEffect", "2"}, {"ContrastAmount", "0.1"}, {"ContrastEffect", "2"}, {"Enable", "false"}, {"IntensityAmount", "0"}, {"IntensityEffect", "0"}}},
        {"Colors:Button", QVariantMap{{"BackgroundAlternate", "227,227,227"}, {"BackgroundNormal", "240,240,240"}, {"DecorationFocus", "51,153,255"}, {"DecorationHover", "153,204,255"}, {"ForegroundActive", "51,153,255"}, {"ForegroundInactive", "140,140,140"}, {"ForegroundLink", "0,0,255"}, {"ForegroundNegative", "218,68,83"}, {"ForegroundNeutral", "198,92,0"}, {"ForegroundNormal", "0,0,0"}, {"ForegroundPositive", "39,174,96"}, {"ForegroundVisited", "128,0,128"}}},
        {"Colors:Complementary", QVariantMap{{"BackgroundAlternate", "59,64,69"}, {"BackgroundNormal", "49,54,59"}, {"DecorationFocus", "30,146,255"}, {"DecorationHover", "61,174,230"}, {"ForegroundActive", "147,206,233"}, {"ForegroundInactive", "175,176,179"}, {"ForegroundLink", "61,174,230"}, {"ForegroundNegative", "231,76,60"}, {"ForegroundNeutral", "253,188,75"}, {"ForegroundNormal", "239,240,241"}, {"ForegroundPositive", "46,204,113"}, {"ForegroundVisited", "61,174,230"}}},
        {"Colors:Selection", QVariantMap{{"BackgroundAlternate", "51,153,255"}, {"BackgroundNormal", "51,153,255"}, {"DecorationFocus", "51,153,255"}, {"DecorationHover", "153,204,255"}, {"ForegroundActive", "255,255,255"}, {"ForegroundInactive", "143,199,255"}, {"ForegroundLink", "0,0,255"}, {"ForegroundNegative", "176,55,69"}, {"ForegroundNeutral", "246,116,0"}, {"ForegroundNormal", "255,255,255"}, {"ForegroundPositive", "23,104,57"}, {"ForegroundVisited", "128,0,128"}}},
        {"Colors:Tooltip", QVariantMap{{"BackgroundAlternate", "77,77,77"}, {"BackgroundNormal", "255,255,225"}, {"DecorationFocus", "51,153,255"}, {"DecorationHover", "153,204,255"}, {"ForegroundActive", "61,174,233"}, {"ForegroundInactive", "140,140,140"}, {"ForegroundLink", "41,128,185"}, {"ForegroundNegative", "218,68,83"}, {"ForegroundNeutral", "246,116,0"}, {"ForegroundNormal", "0,0,0"}, {"ForegroundPositive", "39,174,96"}, {"ForegroundVisited", "127,140,141"}}},
        {"Colors:View", QVariantMap{{"BackgroundAlternate", "247,247,247"}, {"BackgroundNormal", "255,255,255"}, {"DecorationFocus", "51,153,255"}, {"DecorationHover", "153,204,255"}, {"ForegroundActive", "51,153,255"}, {"ForegroundInactive", "140,140,140"}, {"ForegroundLink", "0,0,255"}, {"ForegroundNegative", "218,68,83"}, {"ForegroundNeutral", "246,116,0"}, {"ForegroundNormal", "0,0,0"}, {"ForegroundPositive", "39,174,96"}, {"ForegroundVisited", "128,0,128"}}},
        {"Colors:Window", QVariantMap{{"BackgroundAlternate", "227,227,227"}, {"BackgroundNormal", "240,240,240"}, {"DecorationFocus", "51,153,255"}, {"DecorationHover", "153,204,255"}, {"ForegroundActive", "51,153,255"}, {"ForegroundInactive", "140,140,140"}, {"ForegroundLink", "0,0,255"}, {"ForegroundNegative", "218,68,83"}, {"ForegroundNeutral", "246,116,0"}, {"ForegroundNormal", "0,0,0"}, {"ForegroundPositive", "39,174,96"}, {"ForegroundVisited", "128,0,128"}}},
        {"General", QVariantMap{{"shadeSortColumn", "true"}}},
        {"KDE", QVariantMap{{"contrast", "4"}}},
        {"WM", QVariantMap{{"activeBackground", "153,180,209"}, {"activeBlend", "185,209,234"}, {"activeForeground", "0,0,0"}, {"inactiveBackground", "191,205,219"}, {"inactiveBlend", "215,228,242"}, {"inactiveForeground", "67,78,84"}}},
    };
}
}

namespace AeroThemeContract {
int version() { return 1; }
const QList<AeroThemeField> &fields()
{
    static const QList<AeroThemeField> definitions = {
        {"globalTheme", "kdeglobals", "KDE", "LookAndFeelPackage", "Aero global theme", "authui7"},
        {"plasmaStyle", "plasmarc", "Theme", "name", "Aero Plasma style", "Seven-Black"},
        {"applicationStyle", "kdeglobals", "KDE", "widgetStyle", "Aero application style", "kvantum"},
        {"kvantumTheme", "Kvantum/kvantum.kvconfig", "General", "theme", "Aero Kvantum theme", "Windows7Aero"},
        {"colorScheme", "kdeglobals", "General", "ColorScheme", "Aero color scheme", "Aero"},
        {"iconTheme", "kdeglobals", "Icons", "Theme", "Aero icons", "Windows 7 Aero"},
        {"cursorTheme", "kcminputrc", "Mouse", "cursorTheme", "Aero cursors", "aero-drop"},
        {"decorationLibrary", "kwinrc", "org.kde.kdecoration2", "library", "Aero decoration plugin", "org.smod.smod"},
        {"decorationTheme", "kwinrc", "org.kde.kdecoration2", "theme", "Aero window decorations", "SMOD"},
        {"smodDecorationTheme", "smodrc", "Windeco", "DecorationTheme", "Aero decoration resource", "Aero"},
        {"reflectionTexture", "kwinrc", "Effect-aeroglassblur", "TextureLocation", "Embedded Aero reflection texture", ""},
        {"palette", "kdeglobals", "", "", "Aero application palette", aeroPalette()},
        // Aero baseline misc/xdg/kcminputrc; installed Xcursor image sizes.
        {"cursorSize", "kcminputrc", "Mouse", "cursorSize", "Cursor size", 32, 32, 48, {"32", "48"}},
        // aeroshell-kwin-components 97cc934b, Wayland blur.kcfg + KCM UI.
        {"blurStrength", "kwinrc", "Effect-aeroglassblur", "BlurStrength", "Blur strength", 3, 2, 15},
        {"aeroIntensity", "kwinrc", "Effect-aeroglassblur", "AeroIntensity", "Glass color intensity", 50, 26, 217},
        {"aeroHue", "kwinrc", "Effect-aeroglassblur", "AeroHue", "Glass hue", 0, 0, 359},
        {"aeroSaturation", "kwinrc", "Effect-aeroglassblur", "AeroSaturation", "Glass saturation", 50, 0, 100},
        {"aeroBrightness", "kwinrc", "Effect-aeroglassblur", "AeroBrightness", "Glass brightness", 50, 0, 100},
        {"reflectionIntensity", "kwinrc", "Effect-aeroglassblur", "ReflectionIntensity", "Glass reflection intensity", 75, 0, 100},
        {"enableTransparency", "kwinrc", "Effect-aeroglassblur", "EnableTransparency", "Enable glass transparency", true},
        {"enableCornerGlow", "kwinrc", "Effect-aeroglassblur", "EnableCornerGlow", "Enable glass corner glow", true},
        {"basicColorization", "kwinrc", "Effect-aeroglassblur", "BasicColorization", "Basic glass colorization", false},
        {"maximizeColorization", "kwinrc", "Effect-aeroglassblur", "MaximizeColorization", "Colorize maximized windows", false},
        // These are the KCM's palette selection metadata; HSV/intensity above
        // are the actual shader color. Palette has 8 groups and 128 entries.
        {"accentColorGroup", "kwinrc", "Effect-aeroglassblur", "AccentColorGroup", "Native accent palette group", 0, 0, 7},
        {"accentColorName", "kwinrc", "Effect-aeroglassblur", "AccentColorName", "Native accent palette index", 1, 0, 127},
        {"customColor", "kwinrc", "Effect-aeroglassblur", "CustomColor", "Native custom accent (ARGB)", "#ffffffff"},
        // Installed SMOD breezesettingsdata.kcfg, Common group only.
        {"smodEnableShadow", "smodrc", "Common", "EnableShadow", "Window decoration shadows", true},
        {"smodOutlineCloseButton", "smodrc", "Common", "OutlineCloseButton", "Outline close button", false},
    };
    return definitions;
}
QVariantMap identities()
{
    QVariantMap result;
    for (const auto &field : fields()) {
        if (field.key == QLatin1String("cursorSize")) break;
        result.insert(field.key, field.defaultValue);
    }
    return result;
}
QVariantMap defaults()
{
    QVariantMap result;
    for (const auto &field : fields()) result.insert(field.key, field.defaultValue);
    return result;
}
bool validateRequested(const QVariantMap &values, QString &reason)
{
    reason.clear();
    if (values.size() != fields().size()) {
        reason = QStringLiteral("The complete versioned Aero settings baseline is required."); return false;
    }
    const auto fixed = identities();
    for (const auto &field : fields()) {
        if (!values.contains(field.key)) { reason = QStringLiteral("Missing Aero setting: %1").arg(field.key); return false; }
        const auto value = values.value(field.key);
        bool valid = false;
        if (fixed.contains(field.key))
            valid = value.metaType().id() == field.defaultValue.metaType().id() && value == field.defaultValue;
        else if (field.defaultValue.metaType().id() == QMetaType::Bool)
            valid = value.metaType().id() == QMetaType::Bool;
        else if (field.defaultValue.metaType().id() == QMetaType::Int)
            valid = value.metaType().id() == QMetaType::Int && value.toInt() >= field.minimum
                && value.toInt() <= field.maximum && (field.choices.isEmpty() || field.choices.contains(QString::number(value.toInt())));
        else if (field.key == QLatin1String("customColor"))
            valid = value.metaType().id() == QMetaType::QString
                && QRegularExpression(QStringLiteral("^#[0-9a-fA-F]{8}$")).match(value.toString()).hasMatch();
        if (!valid) { reason = QStringLiteral("Unsupported Aero value for %1.").arg(field.key); return false; }
    }
    return true;
}

namespace {
struct Asset { const char *path; const char *hash; };
const Asset assets[] = {
    {"/usr/share/plasma/look-and-feel/authui7", "63782624ff4709c7bc0e8c287363d80474a47a36c159b959eb2eaaad3926b4f8"},
    {"/usr/share/plasma/desktoptheme/Seven-Black", "a25ab6ed5b586c11e885c888eb48844b020bdda75d3681293d7af2eab9914865"},
    {"/usr/share/icons/Windows 7 Aero", "9cddceb7fa701957bdacd51fe0a05f7e1e262e4134a8ee63bba0d745322f0cb5"},
    {"/usr/share/icons/aero-drop", "5741e5b971685eb1d1626229b6eed1a8ba37b35c01ac237987eb6560d9f228d8"},
    {"/usr/share/Kvantum/Windows7Aero", "c9501ad5d843555ab3d3265fccea9689f70effda52ceb25858690ea22065ca80"},
    {"/usr/share/color-schemes/Aero.colors", "0582996ef882b16afc74a6ed119dd60a2a59a587d6bd292d7a7149c5da6b21f1"},
    {"/usr/share/smod/decorations/Aero.smod.rcc", "338be072250e6ff69c90a6df3837b607b67d7f0c1fb95ad55bca85a83d22b6c8"},
    {"/usr/lib/x86_64-linux-gnu/qt6/plugins/org.kde.kdecoration3/org.smod.smod.so", "f91ade564a16448e1057b61791fa280bb615467a33f3ef53ad15e6d2decc999a"},
    {"/usr/lib/x86_64-linux-gnu/qt6/plugins/styles/libkvantum.so", "d108dd2f1d5a6e3ca15faa81e0e7b532ec8ed5273a419da0a276bd6f212cf419"},
};
QByteArray fileDigest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result().toHex() : QByteArray();
}
QByteArray calculateAssetDigest(const QString &path)
{
    if (!QFileInfo(path).isDir()) return fileDigest(path);
    QDir root(path);
    QStringList names;
    QDirIterator entries(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        names << root.relativeFilePath(entries.next());
        if (names.size() > 20000) return {};
    }
    std::sort(names.begin(), names.end());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &name : names) {
        const QFileInfo info(root.filePath(name));
        QByteArray record;
        if (info.isSymbolicLink()) {
            std::error_code code;
            const auto target = std::filesystem::read_symlink(info.filePath().toStdString(), code);
            if (code) return {};
            record = QByteArray("L\0", 2) + name.toUtf8() + '\0'
                + QByteArray::fromStdString(target.string()) + '\n';
        } else if (info.isDir()) record = QByteArray("D\0", 2) + name.toUtf8() + '\n';
        else {
            const auto digest = fileDigest(info.filePath());
            if (digest.isEmpty()) return {};
            record = QByteArray("F\0", 2) + name.toUtf8() + '\0' + digest + '\n';
        }
        hash.addData(record);
    }
    return hash.result().toHex();
}
}
QByteArray inspectAssetDigest(const QString &absolutePath)
{
    return QFileInfo(absolutePath).isAbsolute() ? calculateAssetDigest(absolutePath) : QByteArray();
}
bool isApprovedAssetResolution(const QStringList &searchRoots, const QString &relativePath,
                               const QString &approvedAbsolutePath, bool rejectAnyAlternative)
{
    if (QFileInfo(relativePath).isAbsolute() || relativePath.split(QLatin1Char('/')).contains(QStringLiteral(".."))
        || !QFileInfo(approvedAbsolutePath).isAbsolute()) return false;
    const QString approved = QFileInfo(approvedAbsolutePath).canonicalFilePath();
    if (approved.isEmpty()) return false;
    bool found = false;
    for (const auto &root : searchRoots) {
        const QFileInfo candidate(QDir(root).filePath(relativePath));
        if (!candidate.exists()) continue;
        if (candidate.canonicalFilePath() != approved) return false;
        found = true;
        if (!rejectAnyAlternative) return true;
    }
    return found;
}
QStringList installedAssetPaths()
{
    QStringList paths;
    for (const auto &asset : assets) paths << QString::fromUtf8(asset.path);
    paths << QLibraryInfo::path(QLibraryInfo::PluginsPath) + QStringLiteral("/kwin/effects/plugins/aeroglassblur.so");
    return paths;
}
bool validateInstalledAssets(QStringList &issues)
{
    issues.clear();
    for (const auto &asset : assets) {
        const QString path = QString::fromUtf8(asset.path);
        if (inspectAssetDigest(path) != QByteArray(asset.hash))
            issues << QStringLiteral("The approved Aero asset is missing or changed: %1").arg(path);
    }
    // The first selected data asset must be the pinned system copy. A same-name
    // user package is not approved simply because the system copy still exists.
    const QStringList relative = {"plasma/look-and-feel/authui7", "plasma/desktoptheme/Seven-Black",
        "icons/Windows 7 Aero", "icons/aero-drop", "Kvantum/Windows7Aero", "color-schemes/Aero.colors",
        "smod/decorations/Aero.smod.rcc"};
    for (const auto &name : relative) {
        const auto selected = QStandardPaths::locate(QStandardPaths::GenericDataLocation, name,
            name.endsWith(QLatin1String(".colors")) || name.endsWith(QLatin1String(".rcc"))
                ? QStandardPaths::LocateFile : QStandardPaths::LocateDirectory);
        if (QFileInfo(selected).canonicalFilePath() != QFileInfo(QStringLiteral("/usr/share/") + name).canonicalFilePath())
            issues << QStringLiteral("An unapproved package shadows the Aero asset: %1").arg(name);
    }
    for (const auto &path : {QDir::homePath() + QStringLiteral("/.icons/aero-drop"),
                            QDir::homePath() + QStringLiteral("/.icons/Windows 7 Aero"),
                            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/Kvantum/Windows7Aero")})
        if (QFileInfo::exists(path)) issues << QStringLiteral("An unapproved local theme shadows Aero: %1").arg(path);
    QLibrary xcursor(QStringLiteral("Xcursor"), 1);
    using LibraryPath = const char *(*)();
    const auto libraryPath = reinterpret_cast<LibraryPath>(xcursor.resolve("XcursorLibraryPath"));
    bool cursorResolved = false;
    if (libraryPath) {
        for (QString path : QString::fromLocal8Bit(libraryPath()).split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
            if (path.startsWith(QStringLiteral("~/"))) path.replace(0, 1, QDir::homePath());
            const QString theme = QDir(path).filePath(QStringLiteral("aero-drop"));
            if (!QFileInfo::exists(theme)) continue;
            cursorResolved = QFileInfo(theme).canonicalFilePath() == QFileInfo(QStringLiteral("/usr/share/icons/aero-drop")).canonicalFilePath();
            break;
        }
    }
    if (!cursorResolved) issues << QStringLiteral("The active Xcursor search path does not resolve the approved Aero cursor package.");
    const QStringList plugins = {QStringLiteral("styles/libkvantum.so"),
        QStringLiteral("org.kde.kdecoration3/org.smod.smod.so"),
        QStringLiteral("kwin/effects/plugins/aeroglassblur.so")};
    for (const auto &relativePlugin : plugins) {
        const QString approved = QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/plugins/") + relativePlugin;
        if (!isApprovedAssetResolution(QCoreApplication::libraryPaths(), relativePlugin, approved, true))
            issues << QStringLiteral("Qt's plugin search path does not resolve only the approved Aero plugin: %1").arg(relativePlugin);
    }
    QString reason;
    const auto plugin = inspectAeroEffectPlugin(QLibraryInfo::path(QLibraryInfo::PluginsPath)
        + QStringLiteral("/kwin/effects/plugins/aeroglassblur.so"));
    if (!validateAeroEffectPlugin(plugin, reason)) issues << reason;
    return issues.isEmpty();
}
} // namespace AeroThemeContract
