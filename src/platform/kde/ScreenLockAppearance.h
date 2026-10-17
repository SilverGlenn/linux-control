#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace ScreenLockAppearance {
struct Evidence {
    bool approvedIdentity = false;
    bool providerVerified = false;
    bool backgroundAvailable = false;
    bool editable = false; // This reader never authorizes a write.
    QString shellId, loginTheme, backgroundPath;
    QString configPath, providerPath;
    QString description, reason;
    QByteArray backgroundDigest;
    QStringList issues;
};
// Bounded injectable file evidence; does not instantiate QML, plugins, settings,
// service proxies, or privileged helpers. Expected digests pin audited provider bytes.
struct Sources {
    QVariantMap themeIdentities;
    QString shellId;
    QString shellDirectory;
    QString authUiDigest, lockScreenDigest;
    QString providerPath, providerDigest;
    QString sddmConfig;
    QStringList dataRoots;
    bool includeGlobalFallback = true; // false only for isolated file fixtures.
};
Evidence inspect(const Sources &sources);
Evidence readCurrent();
}
