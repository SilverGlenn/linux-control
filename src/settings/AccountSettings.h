#pragma once
#include "SettingsSession.h"

struct AccountState {
    qulonglong uid = 0;
    QString userName, realName, objectPath, serviceOwner, iconFile;
    bool editable = false;
    QString unavailableReason;
    bool sameIdentity(const AccountState &other) const
    { return uid == other.uid && userName == other.userName && objectPath == other.objectPath && serviceOwner == other.serviceOwner; }
};
Q_DECLARE_METATYPE(AccountState)
enum class AccountOutcome { Success, Denied, Cancelled, Failed, Uncertain };
Q_DECLARE_METATYPE(AccountOutcome)

class AccountBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent(quint64 token) = 0;
    virtual void authorize(quint64 token) = 0;
    virtual void cancelAuthorization(quint64 token) = 0;
    virtual void writeName(quint64 token, const AccountState &expected, const QString &name) = 0;
signals:
    void readFinished(quint64 token, bool success, const AccountState &state, const QString &error);
    void authorizationFinished(quint64 token, AccountOutcome outcome, const QString &error);
    void writeFinished(quint64 token, AccountOutcome outcome, const QString &error);
    void externalChange();
};

class AccountSettings final : public SettingsSession {
    Q_OBJECT
public:
    explicit AccountSettings(AccountBackend *backend, QObject *parent = nullptr);
    static AccountSettings *instance();
    AccountState account() const { return m_state; }
    QString stagedName() const { return m_staged; }
    bool isReady() const { return m_ready; }
    bool canApply() const { return m_ready && m_state.editable && !m_uncertain; }
    QString unavailableReason() const
    { return m_uncertain ? tr("An earlier request may still complete. Further changes are disabled in this session.") : m_state.unavailableReason; }
    bool canCancel() const;
    AccountOutcome lastOutcome() const { return m_outcome; }
    static bool validName(const QString &name);
public slots:
    void selectName(const QString &name);
    void apply() override;
    void revert() override;
    void refresh();
    void cancel();
private:
    enum class Phase { Idle, Refresh, Preflight, Authorization, FinalPreflight, Write, Readback };
    void beginRead(Phase phase);
    void onRead(quint64 token, bool success, const AccountState &state, const QString &error);
    void onAuthorized(quint64 token, AccountOutcome outcome, const QString &error);
    void onWritten(quint64 token, AccountOutcome outcome, const QString &error);
    void finish(bool applied = false, bool completesApply = false);
    void failApply(const QString &message, AccountOutcome outcome = AccountOutcome::Failed);
    AccountBackend *m_backend;
    AccountState m_state;
    QString m_staged, m_requested, m_writeError;
    quint64 m_token = 0;
    Phase m_phase = Phase::Idle;
    AccountOutcome m_outcome = AccountOutcome::Failed;
    bool m_ready = false, m_pendingRefresh = false, m_uncertain = false, m_reserved = false, m_keepFailure = false;
};
