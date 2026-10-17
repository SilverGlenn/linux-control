#include "ProgramsService.h"
#include "operations/OperationManager.h"
#include <QCoreApplication>
#include <QPointer>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <climits>
#include <algorithm>

ProgramsService::ProgramsService(Gather gather, QObject *parent)
    : QObject(parent), m_gather(gather ? std::move(gather) : &gatherPrograms)
{
}

ProgramsService *ProgramsService::instance()
{
    static QPointer<ProgramsService> service;
    if (!service) service = new ProgramsService({}, QCoreApplication::instance());
    return service;
}

void ProgramsService::load(bool force)
{
    if (m_loading) {
        m_reloadPending |= force;
        return;
    }
    if (m_loaded && !force) return;
    m_loading = true;
    emit changed();
    auto *watcher = new QFutureWatcher<LoadResult>(this);
    connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher]() {
        m_inventory = watcher->result();
        watcher->deleteLater();
        m_loading = false;
        m_loaded = true;
        ++m_revision;
        const bool reload = m_reloadPending;
        m_reloadPending = false;
        emit changed();
        if (reload) load(true);
    });
    watcher->setFuture(QtConcurrent::run(m_gather));
}

void ProgramsService::finishAction(bool success, const QString &message)
{
    m_acting = false;
    m_actionFailed = !success;
    m_actionStatus = message;
    OperationManager::instance()->releaseMutation(this);
    // Even a failed transaction can have changed packages before it failed.
    load(true);
    emit changed();
}

void ProgramsService::startAction(const QString &package, const QString &display,
                                  bool reinstall)
{
    if (m_acting) return;
    static const QRegularExpression valid(R"(^[a-zA-Z0-9][a-zA-Z0-9@._+\-]*$)");
    if (!valid.match(package).hasMatch()) {
        m_actionFailed = true;
        m_actionStatus = QStringLiteral("Invalid package name.");
        emit changed();
        return;
    }
    const QString pacman = QStandardPaths::findExecutable(QStringLiteral("pacman"));
    auto *manager = OperationManager::instance();
    if (pacman.isEmpty() || !manager->acquireMutation(this)) {
        m_actionFailed = true;
        m_actionStatus = pacman.isEmpty()
            ? QStringLiteral("Package actions require pacman and are unavailable here.")
            : QStringLiteral("Another package transaction is already running.");
        emit changed();
        return;
    }
    m_acting = true;
    m_actionFailed = false;
    m_actionStatus = QStringLiteral("%1 %2. You can visit other pages while this runs.")
        .arg(reinstall ? QStringLiteral("Repairing") : QStringLiteral("Uninstalling"), display);
    emit changed();
    if (!reinstall) {
        launchAction(package, display, false, false);
        return;
    }
    auto *probe = manager->startProcess(QStringLiteral("Checking package source"), pacman,
        {QStringLiteral("-Qm"), package}, false, QStringLiteral("programs-source"), nullptr, 30000);
    if (!probe) {
        finishAction(false, QStringLiteral("The package source check could not start."));
        return;
    }
    connect(probe, &ProcessJob::finished, this, [this, probe, package, display]() {
        // pacman uses 1 for an installed package that is not foreign.
        if (probe->exitStatus() != QProcess::NormalExit
            || probe->exitCode() < 0 || probe->exitCode() > 1) {
            finishAction(false, QStringLiteral("The package source could not be checked: %1")
                .arg(probe->errorString().isEmpty()
                    ? QString::fromUtf8(probe->output()).right(1500) : probe->errorString()));
            return;
        }
        launchAction(package, display, true, probe->exitCode() == 0);
    });
}

void ProgramsService::launchAction(const QString &package, const QString &display,
                                   bool reinstall, bool foreign)
{
    QString program;
    QStringList args;
    if (reinstall && foreign) {
        program = QStandardPaths::findExecutable(QStringLiteral("yay"));
        args = {QStringLiteral("-S"), QStringLiteral("--noconfirm"),
            QStringLiteral("--sudo"), QStringLiteral("pkexec"),
            QStringLiteral("--cleanafter"), package};
    } else {
        program = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        args = {QStandardPaths::findExecutable(QStringLiteral("pacman")),
            reinstall ? QStringLiteral("-S") : QStringLiteral("-R"),
            QStringLiteral("--noconfirm"), package};
    }
    if (program.isEmpty() || args.first().isEmpty()) {
        finishAction(false, QStringLiteral("The required package tool is unavailable."));
        return;
    }
    const QString title = QStringLiteral("%1 %2")
        .arg(reinstall ? QStringLiteral("Repair") : QStringLiteral("Uninstall"), display);
    auto *job = OperationManager::instance()->startProcess(title, program, args,
        true, QStringLiteral("package-transaction"), this);
    if (!job) {
        finishAction(false, QStringLiteral("Another package transaction is already running."));
        return;
    }
    connect(job, &ProcessJob::finished, this, [this, job, display, reinstall]() {
        const QString message = job->succeeded()
            ? QStringLiteral("%1 was %2.").arg(display,
                reinstall ? QStringLiteral("reinstalled") : QStringLiteral("uninstalled"))
            : QStringLiteral("The operation did not complete: %1").arg(
                job->errorString().isEmpty() ? QString::fromUtf8(job->output()).right(1500)
                                            : job->errorString());
        finishAction(job->succeeded(), message);
    });
}

// Data gathering
// "6.32 MiB" / "121.00 KiB" / "1.04 GiB" to bytes.
static qint64 parseSize(const QString &text)
{
    const QStringList parts = text.split(' ', Qt::SkipEmptyParts);
    if (parts.size() < 2) return 0;
    const double value = QLocale::c().toDouble(parts[0]);
    const QString unit = parts[1];
    double mult = 1.0;
    if      (unit.startsWith("Ki")) mult = 1024.0;
    else if (unit.startsWith("Mi")) mult = 1024.0 * 1024.0;
    else if (unit.startsWith("Gi")) mult = 1024.0 * 1024.0 * 1024.0;
    else if (unit.startsWith("Ti")) mult = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    return static_cast<qint64>(value * mult);
}

// What we remember about each package's first appearance, read from
// /var/log/pacman.log. (pacman's own "Install Date" field is bumped on every
// upgrade, so it reflects the last touch, not the original install, useless
// here.) `byInstaller` records whether that first install was done by the OS
// installer rather than by the user later on.
struct InstallRecord { QDate date; bool byInstaller = false; };

// Does this pacman command line look like one the OS installer (Calamares) ran,
// rather than an interactive `pacman -S` or an AUR helper? The installer's
// chroot bootstrap and post-install steps carry tell-tale flags that ordinary
// installs never use: a Calamares-root or explicit "-r /" target, --cachedir,
// or the --noprogressbar / --disable-download-timeout pair. We deliberately do
// NOT key on --config or --noconfirm, since yay/paru's `pacman -U` calls use
// those too.
static bool isInstallerCommand(const QString &cmd)
{
    static const QRegularExpression rootFlag(R"((?:^|\s)-r\s*/)");
    return cmd.contains("calamares")
        || cmd.contains("--noprogressbar")
        || cmd.contains("--disable-download-timeout")
        || cmd.contains("--cachedir")
        || rootFlag.match(cmd).hasMatch();
}

// Walk the pacman log, attributing each package's first install to the command
// that ran it. `sawInstaller` reports whether any installer command was seen at
// all (false only for an unusually truncated log); `earliestDay` is the date of
// the very first install line, used as a fallback signal in that case.
static QHash<QString, InstallRecord> readInstallHistory(QDate &earliestDay,
                                                        bool &sawInstaller)
{
    QHash<QString, InstallRecord> hist;
    earliestDay  = QDate();
    sawInstaller = false;

    QFile log("/var/log/pacman.log");
    if (!log.open(QIODevice::ReadOnly | QIODevice::Text))
        return hist;

    static const QRegularExpression runRe(
        R"(^\[[0-9T:+\-]+\] \[PACMAN\] Running '([^']*)')");
    // [2026-01-24T10:00:00+0100] [ALPM] installed code (1.124.0-1.1)
    static const QRegularExpression instRe(
        R"(^\[([0-9-]{10})T[0-9:+\-]+\] \[ALPM\] installed (\S+) )");

    bool curInstaller = false;
    const QString text = QString::fromUtf8(log.readAll());
    const QList<QStringView> lines = QStringView(text).split(u'\n', Qt::SkipEmptyParts);
    for (const QStringView &line : lines) {
        const QString s = line.toString();

        const auto rm = runRe.match(s);
        if (rm.hasMatch()) {                       // a new transaction's command
            curInstaller = isInstallerCommand(rm.captured(1));
            if (curInstaller) sawInstaller = true;
            continue;
        }

        const auto im = instRe.match(s);
        if (!im.hasMatch()) continue;
        const QString pkg = im.captured(2);
        if (hist.contains(pkg)) continue;          // log is chronological: keep first
        InstallRecord rec;
        rec.date        = QDate::fromString(im.captured(1), "yyyy-MM-dd");
        rec.byInstaller = curInstaller;
        hist.insert(pkg, rec);
        if (!earliestDay.isValid() && rec.date.isValid())
            earliestDay = rec.date;
    }
    return hist;
}

// Is this package part of the operating system rather than user-installed
// software? Even among packages added after setup we hide categories whose
// removal would break boot, hardware or the desktop foundation: kernels and
// their headers, firmware, microcode, bootloader/initramfs, GPU/hardware
// drivers, plus the distro's own config and meta packages (cachyos-*) and the
// package managers / AUR helpers the system is run with (pacman, yay, etc).
static bool isSystemPackage(const QString &name)
{
    static const QRegularExpression sysRe(
        "^("
        "base|base-devel"                                  // base meta packages
        "|linux|linux-lts|linux-zen|linux-hardened"        // stock kernels
        "|linux-cachyos.*"                                 // cachyos kernels
        "|.*-headers"                                      // kernel/dev headers
        "|.*firmware"                                      // *-firmware
        "|.*-ucode"                                        // intel/amd microcode
        "|cachyos-.*"                                       // distro config & meta
        "|grub|grub-.*|refind|systemd-boot|efibootmgr|efitools|os-prober"
        "|mkinitcpio.*|dracut|booster|plymouth.*"          // initramfs / splash
        "|xf86-video-.*|xf86-input-.*|.*-dkms"             // hardware drivers
        "|nvidia.*|mesa|lib32-.*mesa.*|egl-wayland|vulkan-.*-driver"
        "|pacman.*|pamac.*|libpamac.*|archlinux-keyring"   // package management
        "|yay|yay-bin|paru|paru-bin|pikaur|trizen|aurman|pacaur|aura|octopi"  // AUR helpers
        ")$",
        QRegularExpression::CaseInsensitiveOption);
    return sysRe.match(name).hasMatch();
}

// The friendly name and icon read from a .desktop file's [Desktop Entry].
struct DesktopEntry { QString name; QString icon; };

// Parse the [Desktop Entry] group of a .desktop file. Returns false unless it
// is a displayable application with a name (skipping NoDisplay/Hidden helper
// entries such as VS Code's URL handler). Only the main "Name="/"Icon=" keys
// are read; localized "Name[xx]=" keys and per-action groups are ignored.
static bool parseDesktopEntry(const QString &path, DesktopEntry &out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    bool inEntry = false, noDisplay = false, hidden = false;
    QString name, icon, type;
    const QString text = QString::fromUtf8(f.readAll());
    const QList<QStringView> lines = QStringView(text).split(u'\n');
    for (const QStringView &lv : lines) {
        const QString line = lv.trimmed().toString();
        if (line.startsWith('[')) {           // a new group begins
            inEntry = (line == "[Desktop Entry]");
            continue;
        }
        if (!inEntry) continue;
        const int eq = line.indexOf('=');
        if (eq < 0) continue;
        const QString key = line.left(eq);
        const QString val = line.mid(eq + 1);
        if      (key == "Name"      && name.isEmpty()) name = val;
        else if (key == "Icon"      && icon.isEmpty()) icon = val;
        else if (key == "Type")       type      = val;
        else if (key == "NoDisplay")  noDisplay = (val.compare("true", Qt::CaseInsensitive) == 0);
        else if (key == "Hidden")     hidden    = (val.compare("true", Qt::CaseInsensitive) == 0);
    }

    if (name.isEmpty() || noDisplay || hidden) return false;
    if (!type.isEmpty() && type != "Application") return false;
    out.name = name;
    out.icon = icon;
    return true;
}

// Publisher lookup
// pacman's "Packager" field only says who *built* the binary, the distro's
// build farm ("CachyOS") for repo packages, "Unknown Packager" for local AUR
// builds, never who actually makes or maintains the software. The Publisher
// column wants the latter, so we consult two better sources and keep the
// Packager-derived text only as a fallback:
//  - repo packages: the AppStream catalog's <developer> for the package
//    (e.g. steam -> "Valve Corporation"),
//  - foreign/AUR packages: the AUR maintainer from the RPC endpoint
//    (e.g. linux-devmgmt -> "actuallyaridan").

// The names of all foreign (not-in-any-repo, i.e. AUR/local) packages.
static QSet<QString> foreignPackages()
{
    QSet<QString> set;
    QProcess p;
    p.start("pacman", { "-Qmq" });
    if (!p.waitForFinished(10000)) return set;
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    const QList<QStringView> lines = QStringView(out).split(u'\n', Qt::SkipEmptyParts);
    for (const QStringView &l : lines)
        set.insert(l.trimmed().toString());
    return set;
}

// Scan one AppStream catalog/metainfo XML document, adding pkgname -> developer
// entries. Handles both the current <developer><name> form and the legacy
// <developer_name> tag, ignoring translated (xml:lang) variants. Existing
// entries win, so the first component naming a package sticks.
static void parseAppStreamXml(const QByteArray &xml, QHash<QString, QString> &devs)
{
    QXmlStreamReader r(xml);
    QStringList pkgs;
    QString devName, legacyName;
    bool inDeveloper = false;
    while (!r.atEnd()) {
        const auto tok = r.readNext();
        if (tok == QXmlStreamReader::EndElement) {
            if (r.name() == u"developer") {
                inDeveloper = false;
            } else if (r.name() == u"component") {
                const QString dev = !devName.isEmpty() ? devName : legacyName;
                if (!dev.isEmpty())
                    for (const QString &p : pkgs)
                        if (!devs.contains(p)) devs.insert(p, dev);
            }
            continue;
        }
        if (tok != QXmlStreamReader::StartElement) continue;

        const auto tag = r.name();
        const bool translated = r.attributes().hasAttribute("xml:lang");
        if (tag == u"component") {
            pkgs.clear(); devName.clear(); legacyName.clear();
            inDeveloper = false;
        }
        else if (tag == u"pkgname") {
            pkgs << r.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
        }
        else if (tag == u"developer") {
            inDeveloper = true;
        }
        else if (tag == u"name" && inDeveloper) {
            const QString t = r.readElementText(QXmlStreamReader::IncludeChildElements);
            if (!translated && devName.isEmpty()) devName = t.simplified();
        }
        else if (tag == u"developer_name") {
            const QString t = r.readElementText(QXmlStreamReader::IncludeChildElements);
            if (!translated && legacyName.isEmpty()) legacyName = t.simplified();
        }
    }
}

// pkgname -> upstream developer, read from the distro's AppStream catalogs
// (the compressed XML shipped by archlinux-appstream-data). Covers every repo
// desktop application; CLI-only packages have no AppStream component and keep
// the fallback. CachyOS rebuilds of Arch packages keep the same pkgname, so
// they match too.
static QHash<QString, QString> readAppStreamDevelopers()
{
    QHash<QString, QString> devs;
    static const QStringList dirs = {
        "/usr/share/swcatalog/xml", "/var/lib/swcatalog/xml",
        "/usr/share/app-info/xml",  "/var/lib/app-info/xml",   // pre-0.16 paths
    };
    for (const QString &dir : dirs) {
        const QFileInfoList files = QDir(dir).entryInfoList(
            { "*.xml", "*.xml.gz" }, QDir::Files);
        for (const QFileInfo &fi : files) {
            QByteArray xml;
            if (fi.fileName().endsWith(".gz")) {
                QProcess gz;
                gz.start("gzip", { "-dc", fi.absoluteFilePath() });
                if (!gz.waitForFinished(15000)) { gz.kill(); continue; }
                xml = gz.readAllStandardOutput();
            } else {
                QFile f(fi.absoluteFilePath());
                if (!f.open(QIODevice::ReadOnly)) continue;
                xml = f.readAll();
            }
            parseAppStreamXml(xml, devs);
        }
    }
    return devs;
}

// pkgname -> AUR maintainer, via batched requests to the AUR info RPC. Shells
// out to curl (pacman itself depends on it) with a short timeout, so an
// offline machine just keeps the fallback text. Orphaned packages report a
// null maintainer and also keep the fallback.
static QHash<QString, QString> fetchAurMaintainers(const QStringList &pkgs)
{
    QHash<QString, QString> out;
    constexpr int batch = 150;                 // keep the GET URL a sane length
    for (int i = 0; i < pkgs.size(); i += batch) {
        QStringList args;
        for (int j = i; j < qMin(i + batch, pkgs.size()); ++j)
            args << "arg[]=" + QString::fromUtf8(QUrl::toPercentEncoding(pkgs[j]));
        const QString url =
            "https://aur.archlinux.org/rpc/v5/info?" + args.join(u'&');

        QProcess curl;
        curl.start("curl", { "-sf", "--max-time", "8", url });
        if (!curl.waitForFinished(10000)) { curl.kill(); continue; }
        const QJsonArray results = QJsonDocument::fromJson(
            curl.readAllStandardOutput()).object().value("results").toArray();
        for (const QJsonValue &v : results) {
            const QJsonObject o = v.toObject();
            const QString name  = o.value("Name").toString();
            const QString maint = o.value("Maintainer").toString();
            if (!name.isEmpty() && !maint.isEmpty()) out.insert(name, maint);
        }
    }
    return out;
}

void ProgramsService::applyPublishers(QList<ProgramInfo> &programs)
{
    if (programs.isEmpty()) return;

    const QSet<QString> foreign = foreignPackages();
    QStringList aurPkgs;
    bool anyNative = false;
    for (const ProgramInfo &p : programs) {
        if (foreign.contains(p.name)) aurPkgs << p.name;
        else                          anyNative = true;
    }

    const QHash<QString, QString> maint = fetchAurMaintainers(aurPkgs);
    const QHash<QString, QString> devs  = anyNative ? readAppStreamDevelopers()
                                                    : QHash<QString, QString>();
    for (ProgramInfo &p : programs) {
        const QString better = foreign.contains(p.name) ? maint.value(p.name)
                                                        : devs.value(p.name);
        if (!better.isEmpty()) p.publisher = better;
    }
}

void ProgramsService::applyFriendlyNames(QList<ProgramInfo> &programs)
{
    for (ProgramInfo &p : programs)
        p.display = p.name;                   // default: the package name itself
    if (programs.isEmpty()) return;

    // One `pacman -Ql` over all packages lists the files each owns; we want the
    // .desktop entries under share/applications/.
    QStringList args{ "-Ql" };
    for (const ProgramInfo &p : programs) args << p.name;
    QProcess q;
    q.start("pacman", args);
    if (!q.waitForFinished(20000)) return;

    QHash<QString, QStringList> desktops;     // pkg -> its .desktop file paths
    const QString out = QString::fromUtf8(q.readAllStandardOutput());
    const QList<QStringView> lines = QStringView(out).split(u'\n', Qt::SkipEmptyParts);
    for (const QStringView &lv : lines) {
        const int sp = lv.indexOf(u' ');
        if (sp < 0) continue;
        const QString path = lv.mid(sp + 1).toString();
        if (path.endsWith(".desktop") && path.contains("/share/applications/"))
            desktops[lv.left(sp).toString()] << path;
    }

    for (ProgramInfo &p : programs) {
        const QStringList files = desktops.value(p.name);
        QString bestName, bestIcon;
        int bestRank = 99, bestLen = INT_MAX;
        for (const QString &file : files) {
            DesktopEntry e;
            if (!parseDesktopEntry(file, e)) continue;
            // Prefer the entry whose filename matches the package, then the
            // shortest name (e.g. "LibreOffice" over "LibreOffice Writer").
            const int rank = (QFileInfo(file).completeBaseName() == p.name) ? 0 : 1;
            const int len  = e.name.size();
            if (rank < bestRank || (rank == bestRank && len < bestLen)) {
                bestRank = rank; bestLen = len;
                bestName = e.name; bestIcon = e.icon;
            }
        }
        if (!bestName.isEmpty()) {
            p.display  = bestName;
            p.iconName = bestIcon;
        }
    }
}

ProgramsService::LoadResult ProgramsService::gatherPrograms()
{
    LoadResult result;
    if (QStandardPaths::findExecutable(QStringLiteral("pacman")).isEmpty()) {
        result.error = QStringLiteral("Package management is unsupported here; this page requires pacman.");
        return result;
    }

    // Force the C locale so the "Install Date" field is the predictable
    // asctime-style "Thu Apr 30 23:14:31 2026" rather than a localized string.
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C");
    p.setProcessEnvironment(env);
    p.start("pacman", {"-Qei"});
    if (!p.waitForFinished(15000)) {
        p.kill();
        result.error = QStringLiteral("The pacman package query timed out.");
        return result;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        result.error = QStringLiteral("The pacman package query failed.");
        return result;
    }

    const QString out = QString::fromUtf8(p.readAllStandardOutput());

    // A package counts as "installed by the user" only if its first install was
    // NOT performed by the OS installer. That filters out everything the
    // installer laid down (the base system and the whole KDE desktop: dolphin,
    // ark, kate, etc) while keeping anything the user added later, even on the
    // very same day, since the test is how it was installed, not when. We still
    // drop anything that looks like a system component even when user-added.
    QDate earliestDay;
    bool sawInstaller = false;
    const QHash<QString, InstallRecord> hist = readInstallHistory(earliestDay, sawInstaller);
    auto userInstalled = [&](const QString &name) -> bool {
        const auto it = hist.constFind(name);
        if (it == hist.constEnd() || !it->date.isValid())
            return false;
        if (sawInstaller)
            return !it->byInstaller;
        // Fallback for a truncated log with no installer commands: treat
        // anything first installed on the earliest logged day as base OS.
        return earliestDay.isValid() && it->date > earliestDay;
    };

    // Parse the "Key   : value" blocks separated by blank lines.
    ProgramInfo cur;
    bool have = false;
    auto flush = [&]() {
        if (have && !cur.name.isEmpty()
            && userInstalled(cur.name) && !isSystemPackage(cur.name))
            result.programs.append(cur);
        cur = ProgramInfo();
        have = false;
    };

    const QList<QStringView> lines = QStringView(out).split(u'\n');
    for (const QStringView &lineView : lines) {
        const QString line = lineView.toString();
        if (line.trimmed().isEmpty()) { flush(); continue; }

        const int colon = line.indexOf(':');
        if (colon < 0) continue;
        const QString key = line.left(colon).trimmed();
        const QString val = line.mid(colon + 1).trimmed();

        if      (key == "Name")           { cur.name = val; have = true; }
        else if (key == "Version")        cur.version = val;
        else if (key == "Installed Size") cur.sizeBytes = parseSize(val);
        else if (key == "Packager") {
            // Fallback publisher only; applyPublishers() replaces it with the
            // real developer/maintainer wherever one is known.
            // "CachyOS <admin@cachyos.org>" -> "CachyOS"; drop the e-mail part.
            const int lt = val.indexOf('<');
            cur.publisher = (lt > 0 ? val.left(lt) : val).trimmed();
            if (cur.publisher.isEmpty() || cur.publisher == "Unknown Packager")
                cur.publisher = "Unknown";
        }
        else if (key == "Install Date") {
            // Collapse runs of spaces (the day is space-padded) before parsing.
            QString s = val.simplified();
            QDateTime dt = QLocale::c().toDateTime(s, "ddd MMM d hh:mm:ss yyyy");
            cur.installed = dt.date();
        }
    }
    flush();

    applyPublishers(result.programs);      // packager -> real developer / AUR maintainer
    applyFriendlyNames(result.programs);   // package name -> .desktop "Name=" where available
    return result;
}

