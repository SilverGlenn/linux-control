#pragma once

#include <QVariantMap>
#include <QStringList>
#include <QList>

// Schema 1: exact locally verified AeroShell/Plasma 6.6.6 assets. Identity
// fields have one permitted value; parameters never select another theme.
struct AeroThemeField {
    QString key, file, group, entry, label;
    QVariant defaultValue;
    int minimum = 0, maximum = 0;
    QStringList choices;
};
namespace AeroThemeContract {
int version();
QVariantMap identities();
QVariantMap defaults();
const QList<AeroThemeField> &fields();
bool validateRequested(const QVariantMap &values, QString &reason);
// Read-only, exact installed asset bytes including sorted relative tree names.
// Never loads a plugin or executes a package script.
bool validateInstalledAssets(QStringList &issues);
QStringList installedAssetPaths();
// Bounded pure read-only helpers, also usable with isolated temporary assets.
QByteArray inspectAssetDigest(const QString &absolutePath);
bool isApprovedAssetResolution(const QStringList &searchRoots, const QString &relativePath,
                               const QString &approvedAbsolutePath, bool rejectAnyAlternative = false);
}
