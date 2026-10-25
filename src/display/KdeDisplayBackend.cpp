#include "KdeDisplayBackend.h"
#include "release/LocalRcPolicy.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>
#include <memory>

#ifndef CONTROL_DISPLAY_GUARD_PATH
#define CONTROL_DISPLAY_GUARD_PATH "/usr/libexec/controlpanel-display-guard"
#endif

KdeDisplayBackend::KdeDisplayBackend(QObject *parent) : DisplayBackend(parent) {}

KdeDisplayBackend::~KdeDisplayBackend()
{
    if (m_reader) {
        m_reader->disconnect(this);
        m_reader->kill(); m_reader->waitForFinished(1000);
    }
    if (m_guard) {
        // QProcess's destructor kills its child. Leave this app-lifetime handle
        // alive during shutdown: EOF asks the independent guard to roll back,
        // and process exit closes the pipe even after a main-process crash.
        m_guard->disconnect(this);
        m_guard->closeWriteChannel();
        if (m_guard->state() == QProcess::NotRunning) delete m_guard;
    }
}

QString KdeDisplayBackend::helperPath() const
{
    const QString installed = QString::fromUtf8(CONTROL_DISPLAY_GUARD_PATH);
    if (QFileInfo(installed).isExecutable()) return installed;
    const QString sibling = QCoreApplication::applicationDirPath() + QStringLiteral("/controlpanel-display-guard");
    return QFileInfo(sibling).isExecutable() ? sibling : QString();
}

bool KdeDisplayBackend::rollbackAvailable() const
{
    return !helperPath().isEmpty() && QFileInfo(QStringLiteral("/usr/bin/kscreen-doctor")).isExecutable();
}

QString KdeDisplayBackend::editRestrictionReason() const
{
    return QString::fromUtf8(release::localRc::displayEditsWithheldReason());
}

void KdeDisplayBackend::readCurrent()
{
    if (m_reader || m_guard) {
        emit readFinished(false, {}, QStringLiteral("Another display operation is in progress."));
        return;
    }
    auto *process = new QProcess(this);
    m_reader = process;
    struct Result { QByteArray out, errors; bool done = false; bool overflow = false; bool timeout = false; };
    auto result = std::make_shared<Result>();
    auto drain = [process, result] {
        const QByteArray chunk = process->readAllStandardOutput();
        if (result->out.size() + chunk.size() > 1024 * 1024) {
            result->overflow = true; process->kill();
        } else result->out += chunk;
        const auto errors=process->readAllStandardError();
        if(result->errors.size()<4096) result->errors += errors.left(4096-result->errors.size());
    };
    auto finish = [this, process, result, drain](bool success) {
        if (result->done) return;
        result->done = true; drain();
        DisplaySnapshot snapshot;
        QString error;
        success = success && !result->timeout && !result->overflow
            && DisplayProtocol::parse(result->out, snapshot, error);
        if (!success && error.isEmpty()) error = result->timeout
            ? QStringLiteral("KScreen display discovery timed out.")
            : !result->errors.trimmed().isEmpty() ? QString::fromUtf8(result->errors.trimmed())
            : QStringLiteral("KScreen could not read the displays. A KDE graphical session is required.");
        m_reader = nullptr; process->deleteLater();
        emit readFinished(success, snapshot, error);
    };
    connect(process, &QProcess::readyReadStandardOutput, this, drain);
    connect(process, &QProcess::readyReadStandardError, this, drain);
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finish(false);
    });
    connect(process, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(code == 0 && status == QProcess::NormalExit);
    });
    QTimer::singleShot(15000, process, [process, result] {
        if (!result->done) { result->timeout = true; process->kill(); }
    });
    process->setProcessEnvironment(DisplayProtocol::doctorEnvironment());
    if (helperPath().isEmpty()) { finish(false); return; }
    process->start(helperPath(), {QStringLiteral("--read")});
}

void KdeDisplayBackend::beginPreview(const DisplaySnapshot &baseline, const DisplaySnapshot &requested)
{
    // Enforce candidate scope at the process boundary, including direct calls.
    // The independent guard's isolated recovery fixtures remain available.
    const QString restriction = editRestrictionReason();
    if (!restriction.isEmpty()) {
        emit transactionFinished(false, false, baseline, restriction);
        return;
    }
    if (m_reader || m_guard || !rollbackAvailable()) {
        emit transactionFinished(false, false, baseline, QStringLiteral("The display rollback helper is unavailable or busy."));
        return;
    }
    // Deliberately parentless; see destructor. The child owns all display writes.
    auto *guard = new QProcess;
    m_guard = guard;
    m_finished = false; m_guardBuffer.clear(); m_finalMessage = {};
    connect(guard, &QProcess::readyReadStandardOutput, this, &KdeDisplayBackend::guardMessage);
    connect(guard, &QProcess::readyReadStandardError, this, [guard] { guard->readAllStandardError(); });
    connect(guard, &QProcess::started, this, [guard, baseline, requested] {
        QJsonObject request{{"baseline", baseline.json}, {"requested", requested.json}};
        guard->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    });
    auto ended = [this, guard] {
        if (m_guard != guard) return;
        guardMessage();
        m_guard = nullptr;
        guard->deleteLater();
        if (m_finished) {
            DisplaySnapshot snapshot;
            QString error;
            DisplayProtocol::parse(QJsonDocument(m_finalMessage.value("snapshot").toObject()).toJson(), snapshot, error);
            emit transactionFinished(m_finalMessage.value("kept").toBool(),
                m_finalMessage.value("restored").toBool(), snapshot,
                m_finalMessage.value("error").toString());
        } else {
            m_finished = true;
            emit transactionFinished(false, false, {},
                QStringLiteral("The display rollback helper stopped unexpectedly. The final display state is unknown; reload the displays."));
        }
    };
    connect(guard, &QProcess::finished, this, [ended](int, QProcess::ExitStatus) { ended(); });
    connect(guard, &QProcess::errorOccurred, this, [ended](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) ended();
    });
    guard->start(helperPath(), {});
}

void KdeDisplayBackend::guardMessage()
{
    if (!m_guard) return;
    m_guardBuffer += m_guard->readAllStandardOutput();
    if (m_guardBuffer.size() > 2 * 1024 * 1024) {
        m_guardBuffer.clear();
        // Closing input triggers rollback. Never kill an armed watchdog.
        m_guard->closeWriteChannel();
        return;
    }
    while (m_guardBuffer.contains('\n')) {
        const int end = m_guardBuffer.indexOf('\n');
        const QByteArray line = m_guardBuffer.left(end);
        m_guardBuffer.remove(0, end + 1);
        const auto obj = QJsonDocument::fromJson(line).object();
        DisplaySnapshot snapshot;
        QString parseError;
        DisplayProtocol::parse(QJsonDocument(obj.value("snapshot").toObject()).toJson(), snapshot, parseError);
        if (obj.value("event").toString() == QStringLiteral("preview") && snapshot.isValid()) {
            emit previewReady(snapshot, obj.value("seconds").toInt());
        } else if (obj.value("event").toString() == QStringLiteral("finished") && !m_finished) {
            m_finished = true;
            m_finalMessage = obj;
        }
    }
}

void KdeDisplayBackend::keepPreview()
{
    if (m_guard) m_guard->write("keep\n");
}
void KdeDisplayBackend::revertPreview()
{
    if (m_guard) m_guard->write("revert\n");
}
