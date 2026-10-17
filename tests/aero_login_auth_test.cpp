#include <QtTest>
#include "platform/kde/AeroLoginBackend.h"
#include "login/AeroLoginProtocol.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <sys/stat.h>
#include <unistd.h>

class AeroLoginAuthTest final:public QObject {
    Q_OBJECT
    QTemporaryDir directory;AeroLoginHelperContext context;QByteArray helper,policy;
    void write(const QString &path,const QByteArray &bytes,bool executable=false)
    {
        QFile::remove(path);QSaveFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(bytes),bytes.size());QVERIFY(file.commit());
        QVERIFY(QFile::setPermissions(path,executable?QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther:QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ReadGroup|QFileDevice::ReadOther));
    }
    QByteArray digest(const QByteArray &bytes){return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();}
    void approve(QJsonObject extra={})
    {
        QJsonObject object{{"version",1},{"protocol",1},{"helperSha256",QString::fromLatin1(digest(helper))},{"policySha256",QString::fromLatin1(digest(policy))}};
        for(auto it=extra.constBegin();it!=extra.constEnd();++it)object.insert(it.key(),it.value());write(context.approvalPath,QJsonDocument(object).toJson(QJsonDocument::Compact));
    }
private slots:
    void init()
    {
        QVERIFY(directory.isValid());QVERIFY(::chmod(QFile::encodeName(directory.path()).constData(),0700)==0);
        context={directory.filePath("helper"),directory.filePath("policy"),directory.filePath("approval.json"),quint32(::getuid()),true};
        helper=QByteArray("\x7f" "ELF",4)+QByteArray("private fixture bytes; never executed");
        policy=QStringLiteral("<policyconfig><action id=\"org.linuxcontrol.aerologin.configure\"><defaults><allow_any>no</allow_any><allow_inactive>no</allow_inactive><allow_active>auth_admin</allow_active></defaults><annotate key=\"org.freedesktop.policykit.exec.path\">%1</annotate></action></policyconfig>").arg(context.helperPath).toUtf8();
        write(context.helperPath,helper,true);write(context.policyPath,policy);approve();
    }
    void exactProtectedApprovalCanBeInspectedWithoutExecution()
    {
        const auto capability=inspectAeroLoginHelper(context);QVERIFY2(capability.available,qPrintable(capability.reason));QCOMPARE(capability.revision.size(),32);
        const auto again=inspectAeroLoginHelper(context);QCOMPARE(again.revision,capability.revision);
    }
    void changedHelperAndPolicyRequireFreshMatchingApproval()
    {
        const auto initial=inspectAeroLoginHelper(context);helper+="modified";write(context.helperPath,helper,true);QVERIFY(!inspectAeroLoginHelper(context).available);approve();const auto changed=inspectAeroLoginHelper(context);QVERIFY(changed.available);QVERIFY(changed.revision!=initial.revision);
        policy.replace("auth_admin","auth_admin_keep");write(context.policyPath,policy);approve();QVERIFY(!inspectAeroLoginHelper(context).available);
    }
    void policyExecActionAndApprovalSchemaAreStrict()
    {
        policy.replace(context.helperPath.toUtf8(),"/usr/bin/unapproved-helper");write(context.policyPath,policy);approve();QVERIFY(!inspectAeroLoginHelper(context).available);
        policy.replace("/usr/bin/unapproved-helper",context.helperPath.toUtf8());policy.replace("org.linuxcontrol.aerologin.configure","org.other.configure");write(context.policyPath,policy);approve();QVERIFY(!inspectAeroLoginHelper(context).available);
        policy.replace("org.other.configure","org.linuxcontrol.aerologin.configure");write(context.policyPath,policy);approve({{"protocol",2}});QVERIFY(!inspectAeroLoginHelper(context).available);approve({{"callerPath","/tmp/override"}});QVERIFY(!inspectAeroLoginHelper(context).available);
    }
    void symlinksWritableAncestorsSetuidAndWrongOwnersAreRefused()
    {
        const auto real=directory.filePath("real-helper");write(real,helper,true);QVERIFY(QFile::remove(context.helperPath));QVERIFY(QFile::link(real,context.helperPath));QVERIFY(!inspectAeroLoginHelper(context).available);
        write(context.helperPath,helper,true);QVERIFY(::chmod(QFile::encodeName(directory.path()).constData(),0777)==0);QVERIFY(!inspectAeroLoginHelper(context).available);QVERIFY(::chmod(QFile::encodeName(directory.path()).constData(),0700)==0);
        QVERIFY(::chmod(QFile::encodeName(context.helperPath).constData(),04755)==0);QVERIFY(!inspectAeroLoginHelper(context).available);QVERIFY(::chmod(QFile::encodeName(context.helperPath).constData(),0755)==0);
        auto wrong=context;wrong.expectedUid++;QVERIFY(!inspectAeroLoginHelper(wrong).available);
    }
    void nonregularMissingAndOversizedInstallationFilesFailClosed()
    {
        QVERIFY(QFile::remove(context.policyPath));QVERIFY(::mkfifo(QFile::encodeName(context.policyPath).constData(),0600)==0);QVERIFY(!inspectAeroLoginHelper(context).available);
        write(context.policyPath,policy);write(context.approvalPath,QByteArray(4097,' '));QVERIFY(!inspectAeroLoginHelper(context).available);QVERIFY(QFile::remove(context.helperPath));QVERIFY(!inspectAeroLoginHelper(context).available);
    }
    void productionPathsAndOwnershipCannotBeOverridden()
    {auto untrusted=context;untrusted.fixture=false;QVERIFY(!inspectAeroLoginHelper(untrusted).available);}
    void authPlanHasOnlyFixedArgumentsAndMinimalEnvironment()
    {
        const auto oldLd=qgetenv("LD_PRELOAD"),oldQt=qgetenv("QT_PLUGIN_PATH");qputenv("LD_PRELOAD","/tmp/untrusted.so");qputenv("QT_PLUGIN_PATH","/tmp/untrusted-qt");
        const QByteArray payload="{\"callerPath\":\"/root/not-an-argument\",\"text\":\"--recover\"}";const auto plan=aeroLoginAuthPlan(payload,false);
        if(oldLd.isNull())qunsetenv("LD_PRELOAD");else qputenv("LD_PRELOAD",oldLd);if(oldQt.isNull())qunsetenv("QT_PLUGIN_PATH");else qputenv("QT_PLUGIN_PATH",oldQt);
        QVERIFY(plan.valid);QCOMPARE(plan.program,QStringLiteral("/usr/bin/pkexec"));QCOMPARE(plan.arguments.size(),3);QCOMPARE(plan.arguments[0],QStringLiteral("--disable-internal-agent"));QVERIFY(plan.arguments[1].startsWith('/'));QCOMPARE(plan.arguments[2],QStringLiteral("--apply"));QCOMPARE(plan.input,payload);
        QCOMPARE(plan.environment.keys().size(),3);QVERIFY(!plan.environment.contains("LD_PRELOAD"));QVERIFY(!plan.environment.contains("QT_PLUGIN_PATH"));QCOMPARE(plan.environment.value("PATH"),QStringLiteral("/usr/sbin:/usr/bin:/sbin:/bin"));QCOMPARE(aeroLoginAuthPlan(payload,true).arguments[2],QStringLiteral("--recover"));
    }
    void emptyAndOversizedRequestsNeverFormLaunchPlans()
    {QVERIFY(!aeroLoginAuthPlan({},false).valid);QVERIFY(!aeroLoginAuthPlan(QByteArray(AeroLoginProtocol::MaximumRequestBytes+1,'x'),false).valid);}
};
QTEST_GUILESS_MAIN(AeroLoginAuthTest)
#include "aero_login_auth_test.moc"
