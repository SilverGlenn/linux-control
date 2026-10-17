#pragma once
#include "settings/AeroLoginSettings.h"
#include <functional>

namespace AeroLoginFiles {
enum class FailurePoint { StageConfig, StageImage, CommitConfig, CommitImage, Readback, Rollback, RollbackConfig, RollbackImage, CrashAfterFirstCommit, CrashAfterSecondCommit, Recovery, JournalSync };
struct Context {
    QString root = QStringLiteral("/");
    quint32 expectedUid = 0;
    // Only explicit isolated fixtures may replace the fixed source hashes/anchor.
    bool fixture = false;
    QMap<QString,QByteArray> approvedHashes; // lowercase SHA-256 hex, keyed by fixed absolute source path.
    std::function<bool(const QByteArray &, QString &)> validateImage;
    // True injects failure. Never populated by a production request.
    std::function<bool(FailurePoint)> fail;
};
struct Result {
    bool success = false;
    bool saved = false;
    bool rolledBack = false;
    bool recoveryRequired = false;
    QString error;
    AeroLoginSnapshot snapshot;
};
// Passive bounded regular-file reads. No image decoder, QML, helper or service.
AeroLoginSnapshot read(const Context &context = {}, QString *error = nullptr);
QStringList watchedPaths(const Context &context = {});
QString themeDirectory(const Context &context = {});
// Caller must already hold administrative authorization. Production Context is
// fixed; image validation is injected by the helper's privilege-dropped child.
Result apply(const Context &context, const AeroLoginSnapshot &expected, const AeroLoginSnapshot &requested);
// Explicit rollback recovery only; never called by read(). Requires a fresh
// snapshot revision and journal states untouched by another writer.
Result recover(const Context &context, const QByteArray &expectedRevision);
}
