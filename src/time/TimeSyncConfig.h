#pragma once

#include "TimeSync.h"
#include <QByteArray>
#include <QStringList>
#include <functional>

namespace TimeSyncConfig {

struct Result {
    bool ok = false;
    QString error;
    QByteArray output;
};

struct Paths {
    QString chronyConfig = QStringLiteral("/etc/chrony/chrony.conf");
    QString chronySource = QStringLiteral("/etc/chrony/sources.d/90-controlpanel.sources");
    QString timesyncdConfig = QStringLiteral("/etc/systemd/timesyncd.conf.d/90-controlpanel.conf");
};

// The executable supplies fixed paths and a real command runner. Tests supply
// temporary files and a fake runner; no test invokes a privileged operation.
using CommandRunner = std::function<Result(const QString &, const QStringList &)>;

struct ServiceState {
    bool active = false;
    QString unitFileState; // enabled, enabled-runtime, or disabled
};

Result configure(TimeSync::Provider provider, const QString &service, bool enabled,
                 const QString &server, const Paths &paths,
                 const CommandRunner &run,
                 const std::optional<ServiceState> &previousState = std::nullopt);
Result refresh(TimeSync::Provider provider, const QString &service,
               const CommandRunner &run);

// Used at the privilege boundary and by the UI before offering the installed
// helper. Reject writable ancestors as well as writable executables/files.
bool trustedPath(const QString &path, bool allowMissingLeaf = false);

} // namespace TimeSyncConfig
