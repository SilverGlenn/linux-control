// Unprivileged, independent transaction watchdog. No user-defined executables,
// shell strings, arbitrary executable overrides, or direct display config files.
#include "DisplayProtocol.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <sys/stat.h>
#include <QJsonDocument>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace {
#ifdef CONTROL_DISPLAY_GUARD_TESTING
#ifndef CONTROL_DISPLAY_TEST_DOCTOR_PATH
#error Test guards must use an isolated fake doctor executable
#endif
const QString DoctorPath = QString::fromUtf8(CONTROL_DISPLAY_TEST_DOCTOR_PATH);
constexpr int ConfirmationMs = 1000;
#else
const QString DoctorPath = QStringLiteral("/usr/bin/kscreen-doctor");
constexpr int ConfirmationMs = 15000;
#endif

bool doctor(const QStringList &arguments, QByteArray &output, QString &error, bool *uncertain = nullptr)
{
    QProcess process;
    process.setProcessEnvironment(DisplayProtocol::doctorEnvironment());
    process.start(DoctorPath, arguments);
    if (!process.waitForStarted(1500)) {
        error = QStringLiteral("KScreen could not be started."); return false;
    }
    QElapsedTimer elapsed; elapsed.start();
    while (process.state() != QProcess::NotRunning && elapsed.elapsed() < 5000) {
        process.waitForFinished(50);
        const QByteArray chunk = process.readAllStandardOutput();
        process.readAllStandardError();
        if (output.size() + chunk.size() > 1024 * 1024) {
            process.kill(); process.waitForFinished(1000);
            if (uncertain) *uncertain = true;
            error = QStringLiteral("KScreen returned too much data."); return false;
        }
        output += chunk;
    }
    if (process.state() != QProcess::NotRunning) {
        process.kill(); process.waitForFinished(1000);
        if (uncertain) *uncertain = true;
        error = QStringLiteral("The KScreen command timed out; its native outcome is uncertain."); return false;
    }
    output += process.readAllStandardOutput();
    if (output.size() > 1024*1024) { if (uncertain) *uncertain=true; error=QStringLiteral("KScreen returned too much data."); return false; }
    const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (!ok) { if (uncertain && process.exitStatus() != QProcess::NormalExit) *uncertain = true;
        error = QStringLiteral("The KScreen command failed."); }
    return ok;
}
// All blocking discovery runs in this independent process, never the GUI.
#ifndef CONTROL_DISPLAY_GUARD_TESTING
QVariant busCall(const QString &service, const QString &path, const QString &interface,
                 const QString &method, const QVariantList &arguments = {})
{
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(arguments);
    const auto reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 1000);
    return reply.type() == QDBusMessage::ReplyMessage && reply.arguments().size() == 1
        ? reply.arguments().first() : QVariant();
}
QString kwinOwner()
{
    return busCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"), {QStringLiteral("org.kde.KWin")}).toString();
}
bool auditedIdentity(QJsonObject &identity, QString &error)
{
    error = QStringLiteral("Guarded display changes require the audited KWin 6.6.6 Wayland and libkscreen 6.6.4 runtime.");
    if (qEnvironmentVariable("XDG_SESSION_TYPE") != QLatin1String("wayland") || qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()) return false;
    const auto owner = kwinOwner(); if (!owner.startsWith(QLatin1Char(':'))) return false;
    const auto uid = busCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixUser"), {owner});
    if (!uid.isValid() || uid.toUInt() != ::geteuid()) return false;
    const auto pid = busCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixProcessID"), {owner}).toUInt();
    if (!pid) return false;
    const auto executable = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
    if (!executable.isEmpty()) { if (executable != QLatin1String("/usr/bin/kwin_wayland")) return false; }
    else {
        // KWin's CAP_SYS_NICE makes /proc/PID/exe unreadable to this same-user
        // client on the audited target. Use readable kernel comm/cmdline plus
        // the pinned bus owner, native version, and protected package path.
        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid)), command(QStringLiteral("/proc/%1/cmdline").arg(pid));
        if (!comm.open(QIODevice::ReadOnly) || !command.open(QIODevice::ReadOnly)
            || comm.read(4096).trimmed()!=QByteArray("kwin_wayland")
            || command.read(32768).split('\0').value(0)!=QByteArray("/usr/bin/kwin_wayland")) return false;
    }
    const auto info = busCall(owner, QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"), QStringLiteral("supportInformation")).toString();
    if (!QRegularExpression(QStringLiteral("^KWin version: 6\\.6\\.6\\s*$"), QRegularExpression::MultilineOption).match(info).hasMatch()
        || !QRegularExpression(QStringLiteral("^Operation Mode: Wayland\\s*$"), QRegularExpression::MultilineOption).match(info).hasMatch()) return false;
    QProcess packages;
    packages.start(QStringLiteral("/usr/bin/dpkg-query"), {QStringLiteral("-W"), QStringLiteral("-f=${Package} ${Version}\n"), QStringLiteral("libkscreen-bin"), QStringLiteral("libkf6screen8"), QStringLiteral("kwin-wayland")});
    if (!packages.waitForFinished(2000)) { packages.kill(); packages.waitForFinished(200); return false; }
    const auto versions = QString::fromUtf8(packages.readAllStandardOutput());
    if (packages.exitStatus() != QProcess::NormalExit || packages.exitCode() != 0) return false;
    for (const auto &package : {QStringLiteral("libkscreen-bin"), QStringLiteral("libkf6screen8"), QStringLiteral("kwin-wayland")}) {
        const QString version = package == QLatin1String("kwin-wayland") ? QStringLiteral("6.6.6") : QStringLiteral("6.6.4");
        if (!QRegularExpression(QStringLiteral("^%1 (?:[0-9]+:)?%2(?:-[^\\s]+)?$").arg(package, QRegularExpression::escape(version)), QRegularExpression::MultilineOption).match(versions).hasMatch()) return false;
    }
    for (const auto &path : {DoctorPath, QStringLiteral("/usr/bin/kwin_wayland")}) {
        struct stat metadata{};
        if (::stat(QFile::encodeName(path).constData(), &metadata) || !S_ISREG(metadata.st_mode) || metadata.st_uid != 0 || (metadata.st_mode & 0022)) return false;
    }
    QProcess ownership;
    ownership.start(QStringLiteral("/usr/bin/dpkg-query"), {QStringLiteral("-S"), DoctorPath, QStringLiteral("/usr/bin/kwin_wayland")});
    if (!ownership.waitForFinished(2000)) { ownership.kill(); ownership.waitForFinished(200); return false; }
    const auto files = QString::fromUtf8(ownership.readAllStandardOutput()).split(QLatin1Char('\n'));
    if (ownership.exitCode() || ownership.exitStatus() != QProcess::NormalExit
        || !files.contains(QStringLiteral("libkscreen-bin: /usr/bin/kscreen-doctor")) || !files.contains(QStringLiteral("kwin-wayland: /usr/bin/kwin_wayland"))) return false;
    if (kwinOwner() != owner) return false;
    identity = {{"owner",owner},{"pid",int(pid)},{"kwinVersion","6.6.6"},{"libkscreenVersion","6.6.4"}};
    error.clear(); return true;
}
#endif
bool readDisplay(DisplaySnapshot &snapshot, QString &error)
{
#ifndef CONTROL_DISPLAY_GUARD_TESTING
    QJsonObject identity;
    if (!auditedIdentity(identity, error)) return false;
#endif
    QByteArray output;
    if (!doctor({QStringLiteral("--json")}, output, error) || !DisplayProtocol::parse(output, snapshot, error)) return false;
#ifndef CONTROL_DISPLAY_GUARD_TESTING
    if (kwinOwner() != identity.value("owner").toString()) { error = QStringLiteral("The compositor owner changed during display discovery."); return false; }
    snapshot.json.insert("controlIdentity", identity);
#endif
    return true;
}
bool writeChanges(const DisplaySnapshot &baseline, const DisplaySnapshot &requested, QString &error, bool &uncertain)
{
    if (!DisplayProtocol::canChange(baseline, requested, error)) return false;
    const auto arguments = DisplayProtocol::changeArguments(baseline, requested);
    if (arguments.isEmpty()) return true;
    QByteArray ignored;
    return doctor(arguments, ignored, error, &uncertain);
}
void publish(const QJsonObject &message)
{
    static bool closed = false;
    if (closed) return;
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    // SIGPIPE is ignored: a dead UI must not terminate the rollback owner.
    const char *data = line.constData();
    qsizetype remaining = line.size();
    while (remaining > 0) {
        const ssize_t n = ::write(STDOUT_FILENO, data, size_t(remaining));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            // A frozen UI must never block the independent deadline. A partial
            // frame cannot be followed by another JSON frame, so close output
            // and continue owning rollback without further notifications.
            ::close(STDOUT_FILENO);
            closed = true;
            break;
        }
        data += n; remaining -= n;
    }
}
void finish(bool kept, bool restored, const DisplaySnapshot &snapshot, const QString &error)
{
    publish({{"event", "finished"}, {"kept", kept}, {"restored", restored},
             {"snapshot", snapshot.json}, {"error", error}});
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGHUP, SIG_IGN);
    const int stdoutFlags = ::fcntl(STDOUT_FILENO, F_GETFL, 0);
    if (stdoutFlags < 0 || ::fcntl(STDOUT_FILENO, F_SETFL, stdoutFlags | O_NONBLOCK) < 0)
        return 2; // No write is permitted without a nonblocking notification pipe.
    if (app.arguments().mid(1) == QStringList{QStringLiteral("--read")}) {
        DisplaySnapshot snapshot; QString error;
        if (!readDisplay(snapshot, error)) { const auto bytes=error.toUtf8(); fwrite(bytes.constData(),1,size_t(bytes.size()),stderr); return 1; }
        const auto bytes=QJsonDocument(snapshot.json).toJson(QJsonDocument::Compact);
        // Read-only mode may have a large frame: the reader is asynchronously
        // draining it. No display mutation or rollback is owned in this mode.
        const int flags=::fcntl(STDOUT_FILENO,F_GETFL,0); ::fcntl(STDOUT_FILENO,F_SETFL,flags & ~O_NONBLOCK);
        return fwrite(bytes.constData(),1,size_t(bytes.size()),stdout)==size_t(bytes.size()) ? 0 : 1;
    }
    if (app.arguments().size()!=1) return 2;
    // A bounded initial request is required before any display write.
    QByteArray input;
    QElapsedTimer requestTimer; requestTimer.start();
    while (!input.contains('\n') && requestTimer.elapsed() < 5000) {
        pollfd fd{STDIN_FILENO, POLLIN | POLLHUP, 0};
        if (::poll(&fd, 1, 100) > 0) {
            char bytes[4096]; const ssize_t n = ::read(STDIN_FILENO, bytes, sizeof bytes);
            if (n <= 0) return 2;
            input.append(bytes, n);
            if (input.size() > 2 * 1024 * 1024 + 4096) return 2;
        }
    }
    const int newline = input.indexOf('\n');
    if (newline < 0) return 2;
    const auto request = QJsonDocument::fromJson(input.left(newline)).object();
    input.remove(0, newline + 1);
    DisplaySnapshot baseline, requested;
    QString error;
    if (!DisplayProtocol::parse(QJsonDocument(request.value("baseline").toObject()).toJson(), baseline, error)) { finish(false,false,{},error); return 2; }
    if (request.contains("requested")) {
        if (!DisplayProtocol::parse(QJsonDocument(request.value("requested").toObject()).toJson(), requested, error)) { finish(false,false,{},error); return 2; }
    } else { // Earlier mode-only private fixture wire contract.
        requested=baseline;
        auto *output=requested.output(request.value("output").toInt());
        if (!output) { finish(false,false,{},QStringLiteral("Unknown output.")); return 2; }
        output->currentMode=request.value("mode").toString();
    }
    requested=DisplayProtocol::normalized(requested);
    if (!DisplayProtocol::canChange(baseline,requested,error)) { finish(false,false,{},error); return 2; }
#ifndef CONTROL_DISPLAY_GUARD_TESTING
    if (!baseline.json.contains("controlIdentity")) { finish(false,false,{},QStringLiteral("The baseline has no audited compositor identity. Reload the displays.")); return 2; }
#endif
    DisplaySnapshot current;
    if (!readDisplay(current, error) || !DisplayProtocol::same(baseline, current)) {
        finish(false, false, current, QStringLiteral("The display configuration changed before applying. Reload the displays."));
        return 2;
    }
    bool done = false;
    bool uncertainWrite = false;
    auto rollback = [&] {
        if (done) return;
        done = true;
        QString restoreError;
        DisplaySnapshot actual;
        if (!readDisplay(actual, restoreError)) {
            finish(false, false, {}, QStringLiteral("The display state could not be read for rollback. %1").arg(restoreError));
        } else if (DisplayProtocol::same(baseline, actual)) {
            finish(false, !uncertainWrite, actual, uncertainWrite ? QStringLiteral("A native display write did not complete reliably. The current state matches the baseline, but outstanding native effects cannot be excluded. Reload the displays.") : QString());
        } else {
            DisplaySnapshot restoration;
            if (!DisplayProtocol::rollbackRequest(baseline, requested, actual, restoration, restoreError)) {
                finish(false,false,actual,restoreError);
            } else {
                // Restore only fields owned by this transaction; the fresh
                // snapshot retains brightness, color, and other native policy.
                const bool wrote = writeChanges(actual, restoration, restoreError, uncertainWrite);
                DisplaySnapshot restored;
                const bool read = readDisplay(restored, restoreError);
                const bool verified = wrote && read && !uncertainWrite && DisplayProtocol::same(baseline, restored);
                finish(false, verified, restored, verified ? QString() : QStringLiteral("The previous display arrangement could not be verified after rollback. %1").arg(restoreError));
            }
        }
        app.quit();
    };
    // The rollback deadline includes the initial write and its readback. Abort
    // before writing if the requesting application has already disappeared.
    pollfd parentPipe{STDIN_FILENO, POLLIN | POLLHUP, 0};
    if (::poll(&parentPipe, 1, 0) > 0 && (parentPipe.revents & POLLHUP)) {
        finish(false, true, current, {}); return 0;
    }
    QElapsedTimer previewClock; previewClock.start();
    const bool wrote = writeChanges(current, requested, error, uncertainWrite);
    const bool read = readDisplay(current, error);
    if (!wrote || !read || !DisplayProtocol::matchesRequested(baseline, requested, current)
        || previewClock.elapsed() >= ConfirmationMs) {
        rollback(); return 1;
    }
    QTimer deadline; deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, rollback);
    deadline.start(qMax(1, ConfirmationMs - int(previewClock.elapsed())));
    QSocketNotifier stdinNotifier(STDIN_FILENO, QSocketNotifier::Read);
    auto processInput = [&] {
        while (input.contains('\n') && !done) {
            const int end = input.indexOf('\n');
            const QByteArray command = input.left(end);
            input.remove(0, end + 1);
            if (command != "keep" || previewClock.elapsed() >= ConfirmationMs) { rollback(); return; }
            DisplaySnapshot verified;
            QString verifyError;
            const bool valid = readDisplay(verified, verifyError)
                && DisplayProtocol::matchesRequested(baseline, requested, verified);
            if (!valid || previewClock.elapsed() >= ConfirmationMs) { rollback(); return; }
            done = true;
            finish(true, false, verified, {});
            app.quit();
        }
    };
    QObject::connect(&stdinNotifier, &QSocketNotifier::activated, &app, [&] {
        char bytes[256]; const ssize_t n = ::read(STDIN_FILENO, bytes, sizeof bytes);
        if (n <= 0 || input.size() + n > 1024) { rollback(); return; }
        input.append(bytes, n); processInput();
    });
    // Reject hotplug/external settings changes during the preview as well.
    QTimer monitor;
    QObject::connect(&monitor, &QTimer::timeout, &app, [&] {
        if (done) return;
        DisplaySnapshot observed;
        QString monitorError;
        if (!readDisplay(observed, monitorError)
            || !DisplayProtocol::matchesRequested(baseline, requested, observed)
            || previewClock.elapsed() >= ConfirmationMs) rollback();
    });
    monitor.start(1500);
    // The UI countdown is conservative; the watchdog's monotonic deadline is
    // authoritative. Bounded doctor calls can delay actual rollback completion.
    publish({{"event", "preview"}, {"snapshot", current.json},
             {"seconds", qMax(1, int((ConfirmationMs - previewClock.elapsed()) / 1000))}});
    QTimer::singleShot(0, &app, processInput);
    return app.exec();
}
