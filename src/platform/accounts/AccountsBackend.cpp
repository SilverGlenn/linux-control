#include "AccountsBackend.h"
// API shapes verified against the installed AccountsService interface XML in
// /usr/share/dbus-1/interfaces/org.freedesktop.Accounts{,.User}.xml and own-data
// action in /usr/share/polkit-1/actions/org.freedesktop.accounts.policy.
// Polkit authorization/cancellation contract:
// https://polkit.pages.freedesktop.org/polkit/eggdbus-interface-org.freedesktop.PolicyKit1.Authority.html
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QTimer>
#include <QPointer>
#include <memory>
#include <QUuid>
#include <QXmlStreamReader>
#include <pwd.h>
#include <unistd.h>

QDBusArgument &operator<<(QDBusArgument &a, const AccountAuthSubject &s)
{ a.beginStructure(); a << s.kind << s.details; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a, AccountAuthSubject &s)
{ a.beginStructure(); a >> s.kind >> s.details; a.endStructure(); return a; }
QDBusArgument &operator<<(QDBusArgument &a, const AccountAuthResult &r)
{ a.beginStructure(); a << r.authorized << r.challenge << r.details; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a, AccountAuthResult &r)
{ a.beginStructure(); a >> r.authorized >> r.challenge >> r.details; a.endStructure(); return a; }
namespace {
const QString managerPath = QStringLiteral("/org/freedesktop/Accounts");
const QString userInterface = QStringLiteral("org.freedesktop.Accounts.User");
const QString authorityPath = QStringLiteral("/org/freedesktop/PolicyKit1/Authority");
const QString authorityInterface = QStringLiteral("org.freedesktop.PolicyKit1.Authority");
AccountsBackend::Options productionOptions()
{
    AccountsBackend::Options options;
    options.uid = ::getuid();
    if (const passwd *pw = ::getpwuid(::getuid())) options.userName = QString::fromLocal8Bit(pw->pw_name);
    return options;
}
bool hasSetRealName(const QString &xml)
{
    if (xml.size() > 1024 * 1024) return false;
    QXmlStreamReader reader(xml);
    bool inUser = false, inMethod = false, found = false;
    QStringList inputs, outputs;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const auto name = reader.name();
            if (name == u"interface") inUser = reader.attributes().value("name") == userInterface;
            else if (inUser && name == u"method") {
                inMethod = reader.attributes().value("name") == u"SetRealName";
                inputs.clear(); outputs.clear();
            } else if (inMethod && name == u"arg") {
                const QString type = reader.attributes().value("type").toString();
                if (reader.attributes().value("direction") == u"out") outputs.append(type); else inputs.append(type);
            }
        } else if (reader.isEndElement()) {
            if (reader.name() == u"method" && inMethod) {
                found = inputs == QStringList{QStringLiteral("s")} && outputs.isEmpty();
                inMethod = false;
            } else if (reader.name() == u"interface") inUser = false;
        }
    }
    return !reader.hasError() && found;
}
QString failure(const QDBusMessage &message)
{ return message.errorName() + ": " + message.errorMessage(); }
}
AccountsBackend::AccountsBackend(QObject *parent)
    : AccountsBackend(QDBusConnection::systemBus(), productionOptions(), parent) {}
AccountsBackend::AccountsBackend(const QDBusConnection &connection, const Options &options, QObject *parent)
    : AccountBackend(parent), m_bus(connection), m_options(options)
{
    qDBusRegisterMetaType<AccountAuthSubject>(); qDBusRegisterMetaType<AccountAuthResult>();
    qDBusRegisterMetaType<QMap<QString, QString>>();
    auto *watcher = new QDBusServiceWatcher(options.accountsService, connection, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &AccountsBackend::ownerChanged);
}
void AccountsBackend::call(const QString &service, const QString &path, const QString &interface,
                           const QString &method, const QVariantList &arguments, int timeout,
                           const std::function<void(const QDBusMessage &)> &done, bool interactive)
{
    QDBusMessage message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(arguments);
    message.setInteractiveAuthorizationAllowed(interactive);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, qMax(1, timeout)), this);
    const auto completed = std::make_shared<bool>(false);
    const QPointer<QDBusPendingCallWatcher> safeWatcher(watcher);
    const auto finish = [completed, safeWatcher, done] {
        if (*completed || !safeWatcher) return;
        *completed = true;
        const QDBusMessage reply = safeWatcher->reply();
        safeWatcher->deleteLater();
        done(reply);
    };
    connect(watcher, &QDBusPendingCallWatcher::finished, this, finish);
    // Invalid/disconnected calls may already be complete before connection.
    // The queued fallback and normal signal share the same exactly-once guard.
    if (watcher->isFinished()) QTimer::singleShot(0, this, finish);
}
void AccountsBackend::readCurrent(quint64 token)
{
    readState([this, token](bool success, const AccountState &state, const QString &error) {
        emit readFinished(token, success, state, error);
    });
}
void AccountsBackend::readState(const std::function<void(bool, const AccountState &, const QString &)> &done)
{
    if (!m_bus.isConnected() || m_options.userName.isEmpty()) {
        QTimer::singleShot(0, this, [done] { done(false, {}, "AccountsService or the current-user identity is unavailable."); });
        return;
    }
    call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
         {m_options.accountsService}, m_options.readTimeoutMs, [this, done](const QDBusMessage &ownerReply) {
        if (ownerReply.type() == QDBusMessage::ErrorMessage || ownerReply.arguments().size() != 1) {
            done(false, {}, "AccountsService is unavailable. " + failure(ownerReply)); return;
        }
        const QString owner = ownerReply.arguments().first().toString();
        if (!owner.startsWith(':')) { done(false, {}, "AccountsService returned an invalid service identity."); return; }
        call(owner, managerPath, "org.freedesktop.Accounts", "FindUserById",
             {QVariant::fromValue<qlonglong>(m_options.uid)}, m_options.readTimeoutMs,
             [this, done, owner](const QDBusMessage &pathReply) {
            if (pathReply.type() == QDBusMessage::ErrorMessage || pathReply.arguments().size() != 1
                || pathReply.arguments().first().metaType() != QMetaType::fromType<QDBusObjectPath>()) {
                done(false, {}, "The current account could not be found. " + failure(pathReply)); return;
            }
            const QString path = qvariant_cast<QDBusObjectPath>(pathReply.arguments().first()).path();
            if (!path.startsWith(managerPath + "/User")) { done(false, {}, "AccountsService returned an invalid account object."); return; }
            call(owner, path, "org.freedesktop.DBus.Properties", "GetAll", {userInterface}, m_options.readTimeoutMs,
                 [this, done, owner, path](const QDBusMessage &propertiesReply) {
                if (propertiesReply.type() == QDBusMessage::ErrorMessage || propertiesReply.arguments().size() != 1) {
                    done(false, {}, "Account properties could not be read. " + failure(propertiesReply)); return;
                }
                const auto properties = qdbus_cast<QVariantMap>(propertiesReply.arguments().first());
                if (properties.value("Uid").metaType().id() != QMetaType::ULongLong
                    || properties.value("UserName").metaType().id() != QMetaType::QString
                    || properties.value("RealName").metaType().id() != QMetaType::QString
                    || properties.value("Uid").toULongLong() != m_options.uid
                    || properties.value("UserName").toString() != m_options.userName) {
                    done(false, {}, "AccountsService identity does not match the current user. No change is allowed."); return;
                }
                AccountState state;
                state.uid = m_options.uid; state.userName = m_options.userName;
                state.realName = properties.value("RealName").toString();
                state.iconFile = properties.value("IconFile").toString();
                state.objectPath = path; state.serviceOwner = owner;
                const bool local = properties.value("LocalAccount").metaType().id() == QMetaType::Bool && properties.value("LocalAccount").toBool();
                const bool system = properties.value("SystemAccount").metaType().id() != QMetaType::Bool || properties.value("SystemAccount").toBool();
                call(owner, path, "org.freedesktop.DBus.Introspectable", "Introspect", {}, m_options.readTimeoutMs,
                     [this, done, state, local, system](const QDBusMessage &introspectionReply) mutable {
                    const bool method = introspectionReply.type() != QDBusMessage::ErrorMessage && introspectionReply.arguments().size() == 1
                        && hasSetRealName(introspectionReply.arguments().first().toString());
                    state.editable = local && !system && method && !m_uncertain;
                    state.unavailableReason = m_uncertain ? "An earlier account request may still complete. Further changes are disabled in this session."
                        : !local || system ? "Editing is available only for the current local, non-system account."
                        : !method ? "AccountsService does not expose the supported name-changing method." : QString();
                    if (m_watchedPath != state.objectPath) {
                        if (!m_watchedPath.isEmpty()) m_bus.disconnect(m_options.accountsService, m_watchedPath, userInterface, "Changed", this, SLOT(userChanged()));
                        m_watchedPath = state.objectPath;
                        m_bus.connect(m_options.accountsService, m_watchedPath, userInterface, "Changed", this, SLOT(userChanged()));
                    }
                    m_lastState = state;
                    done(true, state, {});
                });
            });
        });
    });
}
void AccountsBackend::authorize(quint64 token)
{
    if (m_authorizing) { emit authorizationFinished(token, AccountOutcome::Failed, "Authorization is already in progress."); return; }
    if (!m_lastState.editable || m_uncertain) { emit authorizationFinished(token, AccountOutcome::Failed, "The account is unavailable for editing."); return; }
    m_hasAuthorization = false;
    m_authorizing = true; m_authToken = token;
    const quint64 generation = ++m_authGeneration;
    m_cancelId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString cancelId = m_cancelId;
    const AccountAuthSubject subject {QStringLiteral("system-bus-name"), {{QStringLiteral("name"), m_bus.baseService()}}};
    call(m_options.authorityService, authorityPath, authorityInterface, "CheckAuthorization",
         {QVariant::fromValue(subject), QStringLiteral("org.freedesktop.accounts.change-own-user-data"),
          QVariant::fromValue(QMap<QString, QString>{}), QVariant::fromValue<uint>(1), cancelId},
         m_options.authorizationTimeoutMs, [this, token, generation, cancelId](const QDBusMessage &reply) {
        if (!m_authorizing || generation != m_authGeneration || token != m_authToken) return;
        m_authorizing = false; m_cancelId.clear();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            cancelCheck(cancelId); // Best effort; this never authorizes an account write.
            const QDBusError error(reply);
            const auto outcome = reply.errorName().endsWith("Cancelled") ? AccountOutcome::Cancelled
                : error.type() == QDBusError::AccessDenied || reply.errorName().endsWith("PermissionDenied")
                    ? AccountOutcome::Denied : AccountOutcome::Failed;
            emit authorizationFinished(token, outcome, "Authorization did not complete. " + failure(reply)); return;
        }
        if (reply.arguments().size() != 1 || reply.arguments().first().metaType() != QMetaType::fromType<QDBusArgument>()) {
            emit authorizationFinished(token, AccountOutcome::Failed, "Authorization returned an invalid result."); return;
        }
        const auto result = qdbus_cast<AccountAuthResult>(reply.arguments().first());
        if (result.authorized) {
            m_hasAuthorization = true; m_authorizedToken = token;
            emit authorizationFinished(token, AccountOutcome::Success, {});
        }
        else if (!result.details.value("polkit.dismissed").isEmpty())
            emit authorizationFinished(token, AccountOutcome::Cancelled, "The authorization dialog was dismissed. No account change was sent.");
        else emit authorizationFinished(token, AccountOutcome::Denied, "Authorization was denied. No account change was sent.");
    }, true);
}
void AccountsBackend::cancelCheck(const QString &id)
{
    if (id.isEmpty()) return;
    call(m_options.authorityService, authorityPath, authorityInterface, "CancelCheckAuthorization", {id},
         m_options.readTimeoutMs, [](const QDBusMessage &) {});
}
void AccountsBackend::cancelAuthorization(quint64 token)
{
    if (m_hasAuthorization && token == m_authorizedToken) m_hasAuthorization = false;
    if (!m_authorizing || token != m_authToken) return;
    const auto id = m_cancelId;
    m_authorizing = false; ++m_authGeneration; m_cancelId.clear();
    cancelCheck(id);
}
void AccountsBackend::writeName(quint64 token, const AccountState &expected, const QString &name)
{
    const bool authorized = m_hasAuthorization && token == m_authorizedToken;
    if (authorized) m_hasAuthorization = false;
    if (!authorized || !AccountSettings::validName(name) || !expected.editable || m_uncertain
        || expected.uid != m_options.uid || expected.userName != m_options.userName
        || !expected.sameIdentity(m_lastState) || expected.realName != m_lastState.realName) {
        emit writeFinished(token, AccountOutcome::Failed, "The current-user identity or name preflight is no longer valid."); return;
    }
    // The session performs a fresh read after authorization, and the destination
    // is its unique service owner. A restarted daemon never receives this call.
    call(expected.serviceOwner, expected.objectPath, userInterface, "SetRealName", {name}, m_options.writeTimeoutMs,
         [this, token](const QDBusMessage &reply) {
        if (reply.type() != QDBusMessage::ErrorMessage && reply.arguments().isEmpty()) {
            emit writeFinished(token, AccountOutcome::Success, {}); return;
        }
        const QDBusError error(reply);
        if (error.type() == QDBusError::NoReply || error.type() == QDBusError::Timeout || error.type() == QDBusError::Disconnected) {
            m_uncertain = true;
            emit writeFinished(token, AccountOutcome::Uncertain, "The account request timed out or disconnected and may still complete. Further changes are disabled in this session.");
        } else if (reply.errorName().endsWith("PermissionDenied") || error.type() == QDBusError::AccessDenied) {
            emit writeFinished(token, AccountOutcome::Denied, "AccountsService did not grant authorization. Its permission error does not distinguish denial from a dismissed prompt.");
        } else if (reply.errorName().endsWith("Cancelled")) {
            emit writeFinished(token, AccountOutcome::Cancelled, "AccountsService reported cancellation; the current name will be checked.");
        } else emit writeFinished(token, AccountOutcome::Failed, "AccountsService could not change the name. " + failure(reply));
    }, true);
}
void AccountsBackend::userChanged() { emit externalChange(); }
void AccountsBackend::ownerChanged(const QString &, const QString &, const QString &) { emit externalChange(); }
