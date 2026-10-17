#pragma once

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QString>

class QProcess;
class QTimer;

// Persistent, current-user-only font inventory and management. Tools and data
// directory are injectable so tests never touch the user's fonts or cache.
class UserFontService final : public QObject {
    Q_OBJECT
public:
    struct Family { QString name; QString file; };
    struct Options {
        QString dataDirectory;
        QString listProgram = QStringLiteral("fc-list");
        QString cacheProgram = QStringLiteral("fc-cache");
        int timeoutMs = 10000;
        int outputLimit = 4 * 1024 * 1024;
    };
    explicit UserFontService(const Options &options, QObject *parent = nullptr);
    ~UserFontService() override;
    static UserFontService *instance();
    QList<Family> families() const { return m_families; }
    bool busy() const { return m_busy; }
    bool inventoryValid() const { return m_inventoryValid; }
    QString result() const { return m_result; }
    QString managedDirectory() const;
    bool canRemove(const QString &path) const;
    void refresh();
    void installFont(const QString &source);
    void removeFont(const QString &path);
signals:
    void changed();
private:
    enum class Stage { Idle, Inventory, Cache };
    void startInventory();
    void startProcess(Stage stage);
    void processFinished(bool success, const QString &error);
    void finish();
    void reject(const QString &message);
    bool beginMutation();
    void refreshCache(const QString &successMessage);
    Options m_options;
    QList<Family> m_families;
    QProcess *m_process = nullptr;
    QTimer *m_timer = nullptr;
    QByteArray m_output;
    Stage m_stage = Stage::Idle;
    bool m_busy = false, m_inventoryValid = false, m_mutation = false;
    bool m_failed = false;
    QString m_result, m_processError, m_mutationResult, m_expectedPath;
    bool m_expectedRemoval = false;
};
