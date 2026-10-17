#include "ColorSchemeSettings.h"
#include "AeroThemeContract.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <functional>
#include <memory>

namespace {
class KdeColorSchemeBackend final : public ColorSchemeBackend {
public:
    explicit KdeColorSchemeBackend(QObject *parent = nullptr)
        : ColorSchemeBackend(parent)
    {
        m_configDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        m_configFile = m_configDir + QStringLiteral("/kdeglobals");
        auto changed = [this](const QString &) {
            watchConfig(); // Atomic replacements remove the old file watch.
            m_debounce.start();
        };
        connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
        connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
        m_debounce.setSingleShot(true);
        m_debounce.setInterval(100);
        connect(&m_debounce, &QTimer::timeout, this, [this] { emit externalChange(); });
        watchConfig();
    }

    ~KdeColorSchemeBackend() override
    {
        if (m_process && m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }

    void readCurrent() override
    {
        run(QStringLiteral("kreadconfig6"),
            {QStringLiteral("--file"), QStringLiteral("kdeglobals"),
             QStringLiteral("--group"), QStringLiteral("General"),
             QStringLiteral("--key"), QStringLiteral("ColorScheme")}, 2000,
            [this](bool success, const QString &output, const QString &error) {
                const QString id = output.trimmed();
                emit readFinished(success && !id.isEmpty(), id,
                    success && id.isEmpty()
                        ? QStringLiteral("KDE did not report an active color scheme.") : error);
            });
    }

    void applyScheme(const QString &id) override
    {
        Q_UNUSED(id);
        // The shared Aero transaction is the only production theme writer.
        // A scheme name alone cannot prove approved, versioned assets.
        emit writeFinished(false, QStringLiteral("Use Personalization to apply the validated Aero theme."));
    }

    void cancel() override
    {
        if (m_process) {
            m_process->setProperty("cancelled", true);
            m_process->kill();
        }
    }

private:
    using Completion = std::function<void(bool, const QString &, const QString &)>;
    void watchConfig()
    {
        if (QDir(m_configDir).exists() && !m_watcher.directories().contains(m_configDir))
            m_watcher.addPath(m_configDir);
        if (QFile::exists(m_configFile) && !m_watcher.files().contains(m_configFile))
            m_watcher.addPath(m_configFile);
    }

    void run(const QString &program, const QStringList &arguments, int timeout,
             Completion completion)
    {
        auto *process = new QProcess(this);
        m_process = process;
        struct Output { bool done = false; QByteArray out; QByteArray err; };
        auto output = std::make_shared<Output>();
        // Drain both pipes continuously, retaining only bounded diagnostic text.
        auto drain = [process, output] {
            const QByteArray out = process->readAllStandardOutput();
            const QByteArray err = process->readAllStandardError();
            output->out += out.left(qMax(0, 4096 - int(output->out.size())));
            output->err += err.left(qMax(0, 4096 - int(output->err.size())));
        };
        auto finish = [this, process, output, drain, completion](bool success) {
            if (output->done)
                return;
            output->done = true;
            drain();
            QString error = QString::fromUtf8(output->err).trimmed();
            if (process->property("cancelled").toBool()) {
                success = false;
                error = QStringLiteral("Applying the color scheme was cancelled.");
            } else if (process->property("timedOut").toBool()) {
                success = false;
                error = QStringLiteral("The color scheme command timed out.");
            } else if (!success && error.isEmpty()) {
                error = process->error() == QProcess::FailedToStart
                    ? process->errorString()
                    : QStringLiteral("The color scheme command failed (exit code %1).")
                          .arg(process->exitCode());
            }
            if (m_process == process)
                m_process = nullptr;
            process->deleteLater();
            completion(success, QString::fromUtf8(output->out), error);
        };
        connect(process, &QProcess::readyReadStandardOutput, this, drain);
        connect(process, &QProcess::readyReadStandardError, this, drain);
        connect(process, &QProcess::errorOccurred, this,
                [finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(false);
        });
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [finish](int code, QProcess::ExitStatus status) {
            finish(status == QProcess::NormalExit && code == 0);
        });
        QTimer::singleShot(timeout, process, [process, output] {
            if (!output->done && process->state() != QProcess::NotRunning) {
                process->setProperty("timedOut", true);
                process->kill();
            }
        });
        process->start(program, arguments);
    }

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_configDir;
    QString m_configFile;
    QPointer<QProcess> m_process;
};
}

ColorSchemeSettings *ColorSchemeSettings::instance()
{
    static QPointer<ColorSchemeSettings> session;
    if (!session)
        session = new ColorSchemeSettings(new KdeColorSchemeBackend, QCoreApplication::instance());
    return session;
}

ColorSchemeSettings::ColorSchemeSettings(ColorSchemeBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    backend->setParent(this);
    connect(backend, &ColorSchemeBackend::readFinished,
            this, &ColorSchemeSettings::onReadFinished);
    connect(backend, &ColorSchemeBackend::writeFinished,
            this, &ColorSchemeSettings::onWriteFinished);
    connect(backend, &ColorSchemeBackend::externalChange,
            this, &ColorSchemeSettings::refresh);
    // Publish initial busy state before any view starts accepting selections.
    m_busy = true;
    QTimer::singleShot(0, this, [this] { startRefresh(); });
}

void ColorSchemeSettings::selectScheme(const QString &id)
{
    if (m_busy || !m_ready || id != AeroThemeContract::identities().value(QStringLiteral("colorScheme")).toString())
        return;
    m_staged = id;
    m_dirty = m_staged != m_applied;
    if (!m_dirty)
        m_conflict = false;
    m_error.clear();
    m_result.clear();
    m_hasApplyError = false;
    emit stateChanged();
}

void ColorSchemeSettings::apply()
{
    if (m_busy)
        return;
    if (m_conflict || !m_ready) {
        m_hasApplyError = true;
        m_error = m_conflict
            ? QStringLiteral("KDE colors changed outside Control Panel. Revert to reload them before applying.")
            : QStringLiteral("The current KDE color scheme could not be read. Revert to retry.");
        emit stateChanged();
        emit applyFinished(false);
        return;
    }
    if (!m_dirty) {
        emit applyFinished(true);
        return;
    }
    if (m_staged != AeroThemeContract::identities().value(QStringLiteral("colorScheme")).toString()) {
        m_error = QStringLiteral("Only the approved Aero color scheme can be selected.");
        m_hasApplyError = true;
        emit stateChanged();
        emit applyFinished(false);
        return;
    }
    m_requested = m_staged;
    m_error.clear();
    m_result.clear();
    m_hasApplyError = false;
    m_busy = true;
    m_phase = Phase::Preflight;
    emit stateChanged();
    // Check the baseline immediately before writing too: file-watch events can
    // be delayed, so a pending external edit must not be silently overwritten.
    m_backend->readCurrent();
}

void ColorSchemeSettings::revert()
{
    if (m_busy)
        return;
    m_staged = m_applied;
    m_dirty = false;
    m_conflict = false;
    m_error.clear();
    m_result.clear();
    m_hasApplyError = false;
    startRefresh();
}

void ColorSchemeSettings::refresh()
{
    if (m_busy || m_phase != Phase::Idle) {
        m_refreshPending = true;
        return;
    }
    startRefresh();
}

void ColorSchemeSettings::startRefresh()
{
    m_refreshPending = false;
    m_phase = Phase::Refreshing;
    m_busy = true;
    emit stateChanged();
    m_backend->readCurrent();
}

void ColorSchemeSettings::cancel()
{
    if (m_phase == Phase::Applying)
        m_backend->cancel();
}

void ColorSchemeSettings::onWriteFinished(bool success, const QString &error)
{
    if (m_phase != Phase::Applying)
        return;
    m_writeSucceeded = success;
    m_writeError = error;
    m_phase = Phase::Readback;
    // A process exit is never accepted as proof that the setting changed.
    m_backend->readCurrent();
}

void ColorSchemeSettings::onReadFinished(bool success, const QString &id, const QString &error)
{
    if (m_phase != Phase::Refreshing && m_phase != Phase::Preflight
        && m_phase != Phase::Readback)
        return;
    if (m_phase == Phase::Preflight) {
        if (success && !id.isEmpty() && id == m_applied) {
            m_phase = Phase::Applying;
            m_backend->applyScheme(m_requested);
            return;
        }
        if (success && !id.isEmpty()) {
            m_applied = id;
            m_dirty = m_staged != id;
            m_conflict = m_dirty;
            m_error = m_dirty
                ? QStringLiteral("KDE colors changed outside Control Panel. Revert to reload them before applying.")
                : QString();
            if (!m_dirty)
                m_result = QStringLiteral("The selected color scheme is already active.");
        } else {
            m_ready = false;
            m_conflict = true;
            m_error = error.isEmpty()
                ? QStringLiteral("The current color scheme could not be checked before applying.") : error;
        }
        const bool alreadyApplied = m_ready && !m_dirty && !m_conflict;
        m_hasApplyError = !alreadyApplied;
        finishRefresh(true, alreadyApplied);
        return;
    }
    const bool readback = m_phase == Phase::Readback;
    const bool valid = success && !id.isEmpty();
    const bool wasDirty = m_dirty;
    const bool changed = m_ready && valid && id != m_applied;
    if (valid) {
        m_applied = id;
        m_ready = true;
        if (!wasDirty)
            m_staged = id;
        m_dirty = m_staged != m_applied;
        if (!readback && changed && wasDirty && m_dirty)
            m_conflict = true;
        if (!m_dirty)
            m_conflict = false;
    }

    bool applied = false;
    if (readback) {
        applied = m_writeSucceeded && valid && id == m_requested;
        m_hasApplyError = !applied;
        if (applied) {
            m_error.clear();
            m_conflict = false;
            m_result = QStringLiteral("Color scheme applied.");
        } else {
            m_result.clear();
            m_error = !m_writeSucceeded
                ? (m_writeError.isEmpty() ? QStringLiteral("The color scheme could not be applied.") : m_writeError)
                : !valid
                    ? QStringLiteral("The command completed, but the active color scheme could not be read. %1").arg(error)
                    : QStringLiteral("KDE reports a different active color scheme. Your selection is still staged.");
            if (!valid) {
                // Unknown authoritative state must be reloaded before another write.
                m_ready = false;
                m_conflict = true;
            }
        }
    } else if (!valid) {
        if (!m_hasApplyError)
            m_error = error.isEmpty() ? QStringLiteral("The active KDE color scheme could not be read.") : error;
        m_ready = false;
        if (m_dirty)
            m_conflict = true;
    } else if (!m_hasApplyError) {
        m_error.clear();
    }
    finishRefresh(readback, applied);
}

void ColorSchemeSettings::finishRefresh(bool completesApply, bool success)
{
    m_phase = Phase::Idle;
    m_busy = false;
    // A busy-wait navigation observer can start a retry from stateChanged.
    // Deliver this operation's completion before it can subscribe to the next
    // operation, so the old result cannot be mistaken for that retry's result.
    if (completesApply)
        emit applyFinished(success);
    emit stateChanged();
    if (m_refreshPending)
        QTimer::singleShot(0, this, [this] {
            if (m_refreshPending && !m_busy)
                startRefresh();
        });
}
