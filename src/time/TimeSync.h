#pragma once

#include <QList>
#include <QString>
#include <optional>

namespace TimeSync {

enum class Provider { Unknown, Chrony, Timesyncd };

struct Status {
    Provider provider = Provider::Unknown;
    QString service;
    std::optional<bool> enabled;
    std::optional<bool> synchronized;
    QString activeSource;
    QString configuredServer;
    bool canEditServer = false;
    QString detail;
};

struct ServiceProbe {
    Provider provider = Provider::Unknown;
    QString service;
    bool querySucceeded = false;
    bool installed = false;
    bool active = false;
};

Status readStatus();
QString providerName(Provider provider);
bool validServer(const QString &server);
QString chronySourcesDirectory(const QString &configText);
QString selectedChronySource(const QString &csv);
QString preferredServer(const QString &configText);
ServiceProbe serviceProbeFromOutput(Provider provider, const QString &requestedService,
                                    const QString &output, bool commandOk);
Status statusFromProbes(const QList<ServiceProbe> &services,
                       const QString &timedateOutput, bool timedateOk);

} // namespace TimeSync
