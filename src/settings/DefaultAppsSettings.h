#pragma once
#include "SettingsSession.h"
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QStringList>

// Audited against Plasma Workspace 6.6.6 kcm_componentchooser. Never use an
// arbitrary config key, command line or caller-supplied role as a write target.
enum class DefaultAppRole { Browser, Email, FileManager, Terminal, Map, Dialer,
                            TextEditor, ImageViewer, MusicPlayer, VideoPlayer, PdfViewer, ArchiveManager };
struct DefaultAppRoleInfo {
    DefaultAppRole role;
    QString key, label, anchorMime, category, recommended;
    QStringList mimeTypes;
};
struct DefaultApplication {
    QString id, name, icon, exec, path;
    QStringList mimeTypes, categories;
    QByteArray identity;
    bool terminal = false, showInDesktop = true;
};
struct DefaultAppsPreferences {
    QMap<QString, QString> mimeDefaults;
    QString terminalService;
    bool operator==(const DefaultAppsPreferences &other) const
    { return mimeDefaults == other.mimeDefaults && terminalService == other.terminalService; }
    bool operator!=(const DefaultAppsPreferences &other) const { return !(*this == other); }
};
struct DefaultAppsSnapshot {
    DefaultAppsPreferences preferences;
    QList<DefaultApplication> applications;
    QStringList mimeTypes;
    QByteArray revision;
    bool writable = false;
    QString reason, mimeWriteReason;
    QMap<DefaultAppRole, QString> unavailableRoles;
    QMap<QString, QString> unavailableMimeTypes;
    bool canChangeMime(const QString &mime) const { return mimeWriteReason.isEmpty() && !unavailableMimeTypes.contains(mime); }
    const DefaultApplication *application(const QString &id) const;
};
namespace DefaultAppsTypes {
const QList<DefaultAppRoleInfo> &roles();
const DefaultAppRoleInfo *role(DefaultAppRole role);
bool validMime(const QString &mime);
QString canonicalMime(const QString &mime);
bool supports(const DefaultApplication &app, const QString &mime);
bool eligible(const DefaultApplication &app, const DefaultAppRoleInfo &role);
QString selected(const DefaultAppsPreferences &, const DefaultAppRoleInfo &);
}
Q_DECLARE_METATYPE(DefaultAppsSnapshot)
Q_DECLARE_METATYPE(DefaultAppsPreferences)
enum class DefaultAppsOutcome { Success, Failed, Partial, Uncertain };
Q_DECLARE_METATYPE(DefaultAppsOutcome)
class DefaultAppsBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent(quint64 token) = 0;
    virtual void writeChanges(quint64 token, const DefaultAppsSnapshot &expected,
                              const DefaultAppsPreferences &requested) = 0;
    virtual void cancel() {} // Only unsent work can be canceled.
signals:
    void readFinished(quint64 token, bool success, const DefaultAppsSnapshot &, const QString &error);
    void writeFinished(quint64 token, DefaultAppsOutcome, const DefaultAppsSnapshot &, const QString &error);
    void externalChange();
    void operationPending(const QString &message);
};
class DefaultAppsSettings final : public SettingsSession {
    Q_OBJECT
public:
    static DefaultAppsSettings *instance();
    explicit DefaultAppsSettings(DefaultAppsBackend *backend, QObject *parent = nullptr);
    const DefaultAppsSnapshot &applied() const { return m_applied; }
    const DefaultAppsPreferences &staged() const { return m_staged; }
    bool isReady() const { return m_ready; }
    bool canApply() const { return m_ready && m_applied.writable && !m_conflict; }
    DefaultAppsOutcome lastOutcome() const { return m_outcome; }
public slots:
    void selectRole(DefaultAppRole role, const QString &desktopId);
    void selectMime(const QString &mime, const QString &desktopId);
    void restoreDefaults();
    void refresh();
    void apply() override;
    void revert() override;
private:
    void changed();
    void read();
    void onRead(quint64, bool, const DefaultAppsSnapshot &, const QString &);
    void onWritten(quint64, DefaultAppsOutcome, const DefaultAppsSnapshot &, const QString &);
    DefaultAppsBackend *m_backend;
    DefaultAppsSnapshot m_applied;
    DefaultAppsPreferences m_staged, m_requested;
    quint64 m_token = 0;
    bool m_ready = false, m_writing = false, m_pendingRefresh = false, m_reverting = false, m_operationPending = false;
    DefaultAppsOutcome m_outcome = DefaultAppsOutcome::Failed;
};
