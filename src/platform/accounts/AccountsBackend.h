#pragma once
#include "settings/AccountSettings.h"
#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QMap>
#include <functional>

struct AccountAuthSubject { QString kind; QVariantMap details; };
struct AccountAuthResult { bool authorized = false, challenge = false; QMap<QString, QString> details; };
Q_DECLARE_METATYPE(AccountAuthSubject)
Q_DECLARE_METATYPE(AccountAuthResult)
QDBusArgument &operator<<(QDBusArgument &argument, const AccountAuthSubject &subject);
const QDBusArgument &operator>>(const QDBusArgument &argument, AccountAuthSubject &subject);
QDBusArgument &operator<<(QDBusArgument &argument, const AccountAuthResult &result);
const QDBusArgument &operator>>(const QDBusArgument &argument, AccountAuthResult &result);

// Production defaults always target getuid()/getpwuid(), on the system bus.
// Alternate connection/identity is explicit and exists for isolated fixtures.
class AccountsBackend final : public AccountBackend {
    Q_OBJECT
public:
    struct Options {
        QString accountsService = QStringLiteral("org.freedesktop.Accounts");
        QString authorityService = QStringLiteral("org.freedesktop.PolicyKit1");
        qulonglong uid = 0;
        QString userName;
        int readTimeoutMs = 3000;
        int authorizationTimeoutMs = 120000;
        int writeTimeoutMs = 15000;
    };
    explicit AccountsBackend(QObject *parent = nullptr);
    AccountsBackend(const QDBusConnection &connection, const Options &options, QObject *parent = nullptr);
    void readCurrent(quint64 token) override;
    void authorize(quint64 token) override;
    void cancelAuthorization(quint64 token) override;
    void writeName(quint64 token, const AccountState &expected, const QString &name) override;
private slots:
    void userChanged();
    void ownerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);
private:
    void call(const QString &service, const QString &path, const QString &interface, const QString &method,
              const QVariantList &arguments, int timeoutMs, const std::function<void(const QDBusMessage &)> &done,
              bool interactive = false);
    void readState(const std::function<void(bool, const AccountState &, const QString &)> &done);
    void cancelCheck(const QString &id);
    QDBusConnection m_bus;
    Options m_options;
    AccountState m_lastState;
    QString m_watchedPath, m_cancelId;
    quint64 m_authToken = 0, m_authGeneration = 0, m_authorizedToken = 0;
    bool m_authorizing = false, m_uncertain = false, m_hasAuthorization = false;
};
