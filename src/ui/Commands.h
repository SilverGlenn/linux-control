#pragma once

#include <QString>
#include <QStringList>
#include <QFileInfo>

// All external launch paths use this policy, including links outside the
// catalog. Broad appearance editors can replace the approved Aero components.
inline bool isAeroPolicyBlockedCommand(const QStringList &command)
{
    if (command.isEmpty()) return false;
    const QString program = QFileInfo(command.first()).fileName();
    if (program == QLatin1String("systemsettings") || program == QLatin1String("systemsettings6")
        || program == QLatin1String("lookandfeeltool")
        || program.startsWith(QLatin1String("plasma-apply-"))) return true;
    if (program != QLatin1String("kcmshell6")) return false;
    const QStringList modules{QStringLiteral("kcm_colors"), QStringLiteral("kcm_icons"),
        QStringLiteral("kcm_cursortheme"), QStringLiteral("kcm_style"), QStringLiteral("kcmstyle"),
        QStringLiteral("kcm_lookandfeel"), QStringLiteral("kcm_desktoptheme"),
        QStringLiteral("kcm_kwindecoration"), QStringLiteral("kcm_kwin_effects"),
        QStringLiteral("colors"), QStringLiteral("icons"), QStringLiteral("cursortheme"),
        QStringLiteral("style"), QStringLiteral("lookandfeel"), QStringLiteral("desktoptheme"),
        QStringLiteral("kwindecoration"), QStringLiteral("kwin_effects")};
    for (QString argument : command.mid(1)) {
        argument = QFileInfo(argument).fileName();
        if (argument.endsWith(QLatin1String(".desktop"))) argument.chop(8);
        if (modules.contains(argument)) return true;
    }
    return false;
}

inline QString aeroPolicyCommandReason()
{
    return QStringLiteral("This Control Panel uses the custom Aero theme. Use Personalization to customize Aero or restore its defaults.");
}

// ---------------------------------------------------------------------------
// External launchers shared by MainWindow's task links and the detail pages'
// sidebar links. Task and sidebar links with no dedicated page in this app hand
// off to the real KDE module, the way the Windows Control Panel opens an applet.
// ---------------------------------------------------------------------------

// Command for opening a KDE System Settings module, e.g. kcm("kcm_kscreen").
inline QStringList kcm(const char *module)
{
    return { QStringLiteral("kcmshell6"), QString::fromLatin1(module) };
}

// Desktop Gadgets links hook into KDE Plasma's real widget panels. The
// "Desktop Gadgets" heading and "Add gadgets to the desktop" link toggle
// Plasma's widget explorer (the "Add or Manage widgets" panel) over D-Bus;
// "Get more gadgets online" opens the Get-New-Widgets download dialog.
inline const QStringList kWidgetExplorerCmd = {
    QStringLiteral("qdbus6"), QStringLiteral("org.kde.plasmashell"),
    QStringLiteral("/PlasmaShell"),
    QStringLiteral("org.kde.PlasmaShell.toggleWidgetExplorer")
};
inline const QStringList kGetWidgetsCmd = {
    QStringLiteral("knewstuff-dialog6"),
    QStringLiteral("/usr/share/knsrcfiles/plasmoids.knsrc")
};

// "Device Manager" links launch the standalone devmgmt program.
inline const QStringList kDeviceManagerCmd = {
    QStringLiteral("devmgmt")
};
