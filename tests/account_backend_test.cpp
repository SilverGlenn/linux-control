#include <QTest>
#include <QSignalSpy>
#include <QDBusContext>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QProcess>
#include <QTemporaryDir>
#include <memory>
#include "platform/accounts/AccountsBackend.h"

class FixtureAccounts final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Accounts")
public:
    qlonglong requestedUid = -1;
public slots:
    QDBusObjectPath FindUserById(qlonglong uid)
    { requestedUid = uid; return QDBusObjectPath("/org/freedesktop/Accounts/User1000"); }
};
class FixtureAccountUser final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Accounts.User")
    Q_PROPERTY(qulonglong Uid MEMBER uid)
    Q_PROPERTY(QString UserName MEMBER login)
    Q_PROPERTY(QString RealName MEMBER name)
    Q_PROPERTY(QString IconFile MEMBER icon)
    Q_PROPERTY(bool LocalAccount MEMBER local)
    Q_PROPERTY(bool SystemAccount MEMBER system)
public:
    qulonglong uid = 1000;
    QString login = "fixture", name = "Original Name", icon;
    bool local = true, system = false, deny = false, delay = false, fakeSuccess = false;
    bool interactive = false;
    int writes = 0;
    QString requested;
    QDBusMessage pending;
    QDBusConnection *bus = nullptr;
    void completeLate()
    {
        name = requested; emit Changed(); bus->send(pending.createReply());
    }
public slots:
    void SetRealName(const QString &value)
    {
        ++writes; requested = value; interactive = message().isInteractiveAuthorizationAllowed();
        if (delay) { setDelayedReply(true); pending = message(); return; }
        if (deny) { sendErrorReply("org.freedesktop.Accounts.Error.PermissionDenied", "Fixture denies the request"); return; }
        if (!fakeSuccess) { name = value; emit Changed(); }
    }
signals:
    void Changed();
};
class FixtureReadOnlyUser final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Accounts.User")
    Q_PROPERTY(qulonglong Uid MEMBER uid)
    Q_PROPERTY(QString UserName MEMBER login)
    Q_PROPERTY(QString RealName MEMBER name)
    Q_PROPERTY(bool LocalAccount MEMBER local)
    Q_PROPERTY(bool SystemAccount MEMBER system)
public:
    qulonglong uid = 1000;
    QString login = "fixture", name = "Original Name";
    bool local = true, system = false;
};
class FixtureAuthority final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.PolicyKit1.Authority")
public:
    AccountAuthResult result {true, false, {}};
    AccountAuthSubject subject;
    QString action, cancellationId;
    uint flags = 0;
    bool interactive = false, delay = false;
    int requests = 0, cancellations = 0;
    QDBusMessage pending;
    QDBusConnection *bus = nullptr;
    void completeLate() { bus->send(pending.createReply(QVariant::fromValue(result))); }
public slots:
    AccountAuthResult CheckAuthorization(const AccountAuthSubject &value, const QString &actionId,
                                         const QMap<QString, QString> &, uint authorizationFlags, const QString &id)
    {
        ++requests; subject = value; action = actionId; flags = authorizationFlags; cancellationId = id;
        interactive = message().isInteractiveAuthorizationAllowed();
        if (delay) { setDelayedReply(true); pending = message(); }
        return result;
    }
    void CancelCheckAuthorization(const QString &id)
    { if (id == cancellationId) ++cancellations; }
};
class AccountBackendTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qDBusRegisterMetaType<AccountAuthSubject>(); qDBusRegisterMetaType<AccountAuthResult>();
        qDBusRegisterMetaType<QMap<QString, QString>>();
        QVERIFY(directory.isValid());
        daemon.start("dbus-daemon", {"--session", "--nofork", "--print-address=1",
                     QStringLiteral("--address=unix:path=%1/bus").arg(directory.path())});
        QVERIFY(daemon.waitForStarted(2000)); QVERIFY(daemon.waitForReadyRead(2000));
        const QString address = QString::fromUtf8(daemon.readLine()).trimmed(); QVERIFY(!address.isEmpty());
        server = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address, "accounts-fixture-server"));
        client = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address, "accounts-fixture-client"));
        QVERIFY(server->isConnected()); QVERIFY(client->isConnected());
        QVERIFY(server->registerService("org.freedesktop.Accounts"));
        QVERIFY(server->registerService("org.freedesktop.PolicyKit1"));
        QVERIFY(server->registerObject("/org/freedesktop/Accounts", &manager, exportFlags()));
        QVERIFY(server->registerObject("/org/freedesktop/PolicyKit1/Authority", &authority, exportFlags()));
        authority.bus = server.get(); user.bus = server.get();
    }
    void cleanupTestCase()
    {
        client.reset(); server.reset();
        QDBusConnection::disconnectFromBus("accounts-fixture-client");
        QDBusConnection::disconnectFromBus("accounts-fixture-server");
        daemon.terminate(); if (!daemon.waitForFinished(1000)) { daemon.kill(); daemon.waitForFinished(1000); }
    }
    void init()
    {
        server->unregisterObject("/org/freedesktop/Accounts/User1000");
        QVERIFY(server->registerObject("/org/freedesktop/Accounts/User1000", &user, exportFlags()));
        user.uid = 1000; user.login = "fixture"; user.name = "Original Name";
        user.local = true; user.system = user.deny = user.delay = user.fakeSuccess = false;
        user.writes = 0; user.interactive = false;
        authority.result = {true, false, {}}; authority.delay = false;
        authority.requests = authority.cancellations = 0;
    }
    void exactCurrentUserIdentityAndCapability()
    {
        AccountsBackend backend(*client, options());
        read(backend, 1); QCOMPARE(manager.requestedUid, 1000);
        QCOMPARE(state.userName, "fixture"); QCOMPARE(state.realName, "Original Name"); QVERIFY(state.editable);
        QCOMPARE(state.serviceOwner, server->baseService());
        user.uid = 1001;
        QSignalSpy failed(&backend, &AccountBackend::readFinished); backend.readCurrent(2);
        QTRY_COMPARE(failed.size(), 1); QVERIFY(!failed.first()[1].toBool());
        QVERIFY(failed.first()[3].toString().contains("identity"));
        user.uid = 1000; user.login = "other";
        backend.readCurrent(3); QTRY_COMPARE(failed.size(), 2); QVERIFY(!failed.last()[1].toBool());
        user.login = "fixture"; user.local = false;
        read(backend, 4); QVERIFY(!state.editable);
    }
    void explicitOwnDataAuthorizationAndVerifiedWrite()
    {
        AccountsBackend backend(*client, options()); read(backend, 5);
        QSignalSpy authorized(&backend, &AccountBackend::authorizationFinished);
        backend.authorize(5); QTRY_COMPARE(authorized.size(), 1);
        QCOMPARE(qvariant_cast<AccountOutcome>(authorized.first()[1]), AccountOutcome::Success);
        QCOMPARE(authority.subject.kind, "system-bus-name");
        QCOMPARE(authority.subject.details.value("name").toString(), client->baseService());
        QCOMPARE(authority.action, "org.freedesktop.accounts.change-own-user-data");
        QCOMPARE(authority.flags, 1u); QVERIFY(authority.interactive); QVERIFY(!authority.cancellationId.isEmpty());
        read(backend, 5);
        QSignalSpy written(&backend, &AccountBackend::writeFinished);
        backend.writeName(5, state, "New Name"); QTRY_COMPARE(written.size(), 1);
        QCOMPARE(qvariant_cast<AccountOutcome>(written.first()[1]), AccountOutcome::Success);
        QCOMPARE(user.writes, 1); QVERIFY(user.interactive);
        read(backend, 6); QCOMPARE(state.realName, "New Name");
        backend.writeName(6, state, "Unauthorized followup"); QTRY_COMPARE(written.size(), 2);
        QCOMPARE(qvariant_cast<AccountOutcome>(written.last()[1]), AccountOutcome::Failed); QCOMPARE(user.writes, 1);
    }
    void deniedDismissedAndCanceledChecksStayDistinct()
    {
        AccountsBackend backend(*client, options()); read(backend, 7);
        QSignalSpy authorized(&backend, &AccountBackend::authorizationFinished);
        authority.result = {false, false, {}};
        backend.authorize(7); QTRY_COMPARE(authorized.size(), 1);
        QCOMPARE(qvariant_cast<AccountOutcome>(authorized.last()[1]), AccountOutcome::Denied);
        authority.result = {false, false, {{"polkit.dismissed", "true"}}};
        backend.authorize(8); QTRY_COMPARE(authorized.size(), 2);
        QCOMPARE(qvariant_cast<AccountOutcome>(authorized.last()[1]), AccountOutcome::Cancelled);
        authority.result = {true, false, {}}; authority.delay = true;
        backend.authorize(9); QTRY_COMPARE(authority.requests, 3);
        backend.cancelAuthorization(9); QTRY_COMPARE(authority.cancellations, 1);
        authority.completeLate();
        // A subsequent round trip ensures the late successful reply was processed.
        read(backend, 10); QCOMPARE(authorized.size(), 2);
        QSignalSpy written(&backend, &AccountBackend::writeFinished);
        backend.writeName(9, state, "Must never be sent"); QTRY_COMPARE(written.size(), 1);
        QCOMPARE(user.writes, 0);
    }
    void conflictDenialAndMissingCapabilityNeverSucceed()
    {
        AccountsBackend backend(*client, options()); read(backend, 11);
        const auto stale = state;
        QSignalSpy authorized(&backend, &AccountBackend::authorizationFinished);
        backend.authorize(11); QTRY_COMPARE(authorized.size(), 1);
        user.name = "External"; read(backend, 11);
        QSignalSpy written(&backend, &AccountBackend::writeFinished);
        backend.writeName(11, stale, "Must not overwrite"); QTRY_COMPARE(written.size(), 1);
        QCOMPARE(user.writes, 0); QCOMPARE(qvariant_cast<AccountOutcome>(written.last()[1]), AccountOutcome::Failed);
        backend.authorize(12); QTRY_COMPARE(authorized.size(), 2); read(backend, 12); user.deny = true;
        backend.writeName(12, state, "Denied"); QTRY_COMPARE(written.size(), 2);
        QCOMPARE(qvariant_cast<AccountOutcome>(written.last()[1]), AccountOutcome::Denied); QCOMPARE(user.name, "External");
        server->unregisterObject("/org/freedesktop/Accounts/User1000");
        FixtureReadOnlyUser readOnly;
        QVERIFY(server->registerObject("/org/freedesktop/Accounts/User1000", &readOnly, exportFlags()));
        read(backend, 13); QVERIFY(!state.editable); QVERIFY(state.unavailableReason.contains("method"));
    }
    void writeTimeoutLocksOutLateWriteRaces()
    {
        auto opts = options(); opts.writeTimeoutMs = 50;
        AccountsBackend backend(*client, opts); read(backend, 14);
        QSignalSpy authorized(&backend, &AccountBackend::authorizationFinished);
        backend.authorize(14); QTRY_COMPARE(authorized.size(), 1); read(backend, 14);
        user.delay = true;
        QSignalSpy written(&backend, &AccountBackend::writeFinished);
        backend.writeName(14, state, "Late Name");
        QTRY_COMPARE_WITH_TIMEOUT(written.size(), 1, 3000);
        QCOMPARE(qvariant_cast<AccountOutcome>(written.last()[1]), AccountOutcome::Uncertain);
        read(backend, 15); QVERIFY(!state.editable); QVERIFY(state.unavailableReason.contains("may still complete"));
        backend.authorize(15); QTRY_COMPARE(authorized.size(), 2);
        QCOMPARE(qvariant_cast<AccountOutcome>(authorized.last()[1]), AccountOutcome::Failed);
        backend.writeName(15, state, "Future Name"); QTRY_COMPARE(written.size(), 2); QCOMPARE(user.writes, 1);
        user.completeLate(); read(backend, 16);
        QCOMPARE(state.realName, "Late Name"); QVERIFY(!state.editable); QCOMPARE(written.size(), 2);
    }
    void immediateAuthorizationErrorCompletesExactlyOnce()
    {
        auto opts = options();
        opts.authorityService = QStringLiteral("invalid/service");
        opts.authorizationTimeoutMs = 50;
        AccountsBackend backend(*client, opts);
        read(backend, 23);
        QSignalSpy finished(&backend, &AccountBackend::authorizationFinished);
        backend.authorize(23);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(qvariant_cast<AccountOutcome>(finished.first()[1]), AccountOutcome::Failed);
        // Process another full round trip to catch duplicate signal/fallback delivery.
        read(backend, 24);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(user.writes, 0);
    }
    void disconnectedReadCompletesExactlyOnce()
    {
        const QDBusConnection disconnected(QStringLiteral("account-backend-disconnected"));
        QVERIFY(!disconnected.isConnected());
        AccountsBackend backend(disconnected, options());
        QSignalSpy finished(&backend, &AccountBackend::readFinished);
        backend.readCurrent(25);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QVERIFY(!finished.first()[1].toBool());
        QCoreApplication::processEvents();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(user.writes, 0);
    }
    void missingServiceAndAuthorizationTimeoutAreBounded()
    {
        auto opts = options(); opts.authorizationTimeoutMs = 50;
        AccountsBackend backend(*client, opts); read(backend, 17);
        authority.delay = true;
        QSignalSpy authorized(&backend, &AccountBackend::authorizationFinished);
        backend.authorize(17); QTRY_COMPARE_WITH_TIMEOUT(authorized.size(), 1, 3000);
        QCOMPARE(qvariant_cast<AccountOutcome>(authorized.first()[1]), AccountOutcome::Failed);
        QTRY_COMPARE(authority.cancellations, 1); authority.completeLate();
        QVERIFY(server->unregisterService("org.freedesktop.Accounts"));
        QSignalSpy failed(&backend, &AccountBackend::readFinished); backend.readCurrent(18);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000); QVERIFY(!failed.first()[1].toBool());
        QCOMPARE(user.writes, 0);
        QVERIFY(server->registerService("org.freedesktop.Accounts"));
    }
private:
    static QDBusConnection::RegisterOptions exportFlags()
    { return QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals | QDBusConnection::ExportAllProperties; }
    AccountsBackend::Options options() const
    { AccountsBackend::Options opts; opts.uid = 1000; opts.userName = "fixture"; opts.readTimeoutMs = 500; return opts; }
    void read(AccountsBackend &backend, quint64 token)
    {
        QSignalSpy finished(&backend, &AccountBackend::readFinished);
        backend.readCurrent(token); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QVERIFY2(finished.first()[1].toBool(), qPrintable(finished.first()[3].toString()));
        state = qvariant_cast<AccountState>(finished.first()[2]);
    }
    QTemporaryDir directory;
    QProcess daemon;
    std::unique_ptr<QDBusConnection> server, client;
    FixtureAccounts manager;
    FixtureAccountUser user;
    FixtureAuthority authority;
    AccountState state;
};
QTEST_GUILESS_MAIN(AccountBackendTest)
#include "account_backend_test.moc"
