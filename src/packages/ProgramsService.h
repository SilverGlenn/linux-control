#pragma once

#include <QObject>
#include <QDate>
#include <QList>
#include <QString>
#include <functional>

// The inventory and transactions outlive any Programs and Features view.
class ProgramsService final : public QObject {
    Q_OBJECT
public:
    struct ProgramInfo {
        QString name, display, iconName, version, publisher;
        QDate installed;
        qint64 sizeBytes = 0;
    };
    struct LoadResult { QList<ProgramInfo> programs; QString error; };
    using Gather = std::function<LoadResult()>;
    static ProgramsService *instance();
    explicit ProgramsService(Gather gather = {}, QObject *parent = nullptr);
    bool isLoading() const { return m_loading; }
    bool isActing() const { return m_acting; }
    bool hasInventory() const { return m_loaded; }
    int inventoryRevision() const { return m_revision; }
    LoadResult inventory() const { return m_inventory; }
    QString actionStatus() const { return m_actionStatus; }
    bool actionFailed() const { return m_actionFailed; }
    void load(bool force = false);
    void startAction(const QString &package, const QString &display, bool reinstall);
signals:
    void changed();
private:
    static LoadResult gatherPrograms();
    static void applyPublishers(QList<ProgramInfo> &programs);
    static void applyFriendlyNames(QList<ProgramInfo> &programs);
    void launchAction(const QString &package, const QString &display,
                      bool reinstall, bool foreign);
    void finishAction(bool success, const QString &message);
    Gather m_gather;
    int m_revision = 0;
    LoadResult m_inventory;
    bool m_loading = false, m_loaded = false, m_acting = false;
    bool m_actionFailed = false, m_reloadPending = false;
    QString m_actionStatus;
};
