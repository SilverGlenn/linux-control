#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QSettings>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include "login/AeroLoginFiles.h"
struct AeroLoginDecoderCanary { int value=7; };
static int canaryDecodes=0;
QDataStream &operator<<(QDataStream &stream,const AeroLoginDecoderCanary &value){return stream<<value.value;}
QDataStream &operator>>(QDataStream &stream,AeroLoginDecoderCanary &value){++canaryDecodes;return stream>>value.value;}
Q_DECLARE_METATYPE(AeroLoginDecoderCanary)
using namespace AeroLoginFiles;
namespace {
const QString Theme=QStringLiteral("/usr/share/sddm/themes/sddm-theme-mod");
const QString User=Theme+"/theme.conf.user",Image=Theme+"/background",Default=Theme+"/default-background",Journal=Theme+"/.controlpanel-aero-login.journal";
const QString Main=Theme+"/Main.qml",Metadata=Theme+"/metadata.desktop",Auth="/usr/share/plasma/shells/io.gitgud.wackyideas.desktop/contents/lockscreen/AuthUI.qml",Provider="/usr/lib/x86_64-linux-gnu/qt6/qml/aeroshell/utils/libaeroshellutilsplugin.so";
QByteArray digest(const QByteArray &b){return QCryptographicHash::hash(b,QCryptographicHash::Sha256);}
struct Fixture {
    QTemporaryDir dir;Context c;int validations=0;
    QString p(const QString &relative) const{return dir.filePath(relative.mid(1));}
    bool put(const QString &relative,const QByteArray &bytes){
        const auto name=p(relative);auto parent=QFileInfo(name).absolutePath();if(!QDir().mkpath(parent))return false;
        // Explicit isolated-fixture permissions; never chmod outside its anchor.
        while(parent!=dir.path()){
            if(!parent.startsWith(dir.path()+QLatin1Char('/'))||::chmod(QFile::encodeName(parent).constData(),0755)!=0)return false;
            parent=QFileInfo(parent).absolutePath();
        }
        QFile f(name);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())return false;
        return f.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ReadGroup|QFileDevice::ReadOther);
    }
    QByteArray get(const QString &relative) const{QFile f(p(relative));return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();}
    explicit Fixture(bool image=true){
        c.root=dir.path();c.fixture=true;c.expectedUid=::geteuid();
        for(const auto &name:{Main,Metadata,Auth,Provider}){put(name,"fixture fixed source "+name.toUtf8());c.approvedHashes[name]=digest(get(name)).toHex();}
        put(Theme+"/theme.conf","[General]\nenableStartup=false\nplaySound=true\nforceUserSelect=false\nrdpBackground=false\nkeepDefault=yes\n");
        put("/etc/sddm.conf.d/kde_settings.conf","[Theme]\nCurrent=sddm-theme-mod\n");
        put(Default,"approved default image fixture");c.approvedHashes[Default]=digest(get(Default)).toHex();
        if(image)put(Image,"existing background fixture");
        c.validateImage=[this](const QByteArray &b,QString &error){++validations;if(b.isEmpty()||b.startsWith("invalid")){error="Injected validator rejected image";return false;}return true;};
    }
    AeroLoginSnapshot snapshot(){return read(c);}
    AeroLoginImage picture(){QImage image(2,2,QImage::Format_RGB32);image.fill(Qt::red);QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG");return AeroLoginTypes::prepareImage(bytes);}
};
}
class AeroLoginFilesTest final:public QObject {
    Q_OBJECT
private slots:
    void boundedPassiveReadNativeOverlayAndEditorDisagreement(){
        Fixture f(false);QVERIFY(f.dir.isValid());QVERIFY(f.put(User,"[General]\nenableStartup=off\nplaySound=\nforceUserSelect=no\n[Foreign]\nserialized=@Variant(opaque bytes must never be decoded)\n"));
        const auto before=f.get(User);const auto s=f.snapshot();QVERIFY2(s.sourceValid,qPrintable(s.reason));QVERIFY(s.trustedInstallation&&s.writable);QCOMPARE(s.fields.size(),4);QVERIFY(!s.helperAvailable);
        const auto animation=s.fields[AeroLoginKey::StartupAnimation];QVERIFY(animation.value);QVERIFY(!animation.editorValue);QVERIFY(animation.localPresent&&!animation.canonical);
        QVERIFY(s.fields[AeroLoginKey::StartupSound].userEmpty);QVERIFY(s.fields[AeroLoginKey::StartupSound].value);QVERIFY(!s.background.present);
        QCOMPARE(f.get(User),before);QVERIFY(!QFileInfo::exists(f.p(Journal)));QVERIFY(!QFileInfo::exists(f.p(Theme+"/.controlpanel-aero-login.lock")));
    }
    void scalarOnlyPreservesAbsentImageAndForeignBytes(){
        Fixture f(false);QVERIFY(f.put(User,"; preserve comment\n[General]\nplay%53ound=true ; keep inline description\nforeign=@Variant(never decode this)\n[%General]\nplaySound=nested-unowned\n"));
        const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));const auto result=apply(f.c,b,d);
        QVERIFY2(result.success,qPrintable(result.error));QVERIFY(result.saved);QVERIFY(!QFileInfo::exists(f.p(Image)));QCOMPARE(f.validations,0);
        const auto bytes=f.get(User);QVERIFY(bytes.contains("; preserve comment"));QVERIFY(bytes.contains("; keep inline description"));QVERIFY(bytes.contains("foreign=@Variant(never decode this)"));QVERIFY(bytes.contains("[%General]\nplaySound=nested-unowned"));QVERIFY(!bytes.contains("play%53ound"));QVERIFY(bytes.contains("playSound=false"));
        QVERIFY(!QFileInfo::exists(f.p(Journal)));QVERIFY(!result.snapshot.recoveryPending);
    }
    void exactQtPrefixesKeepOpaqueForeignAssignmentsSafeAndUnchanged(){
        Fixture f;
        const QByteArray foreign="#foreign=@Variant(opaque hash key)\n\v[foreign]=@Variant(opaque VT key)\n\f[foreign]=@Variant(opaque FF key)\n";
        QVERIFY(f.put(User,"[General]\n"+foreign+"playSound=true\n"));const auto b=f.snapshot();
        QVERIFY2(b.sourceValid,qPrintable(b.reason));auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));
        const auto r=apply(f.c,b,d);QVERIFY2(r.success,qPrintable(r.error));QVERIFY(f.get(User).contains(foreign));QVERIFY(f.get(User).contains("playSound=false"));
    }
    void bomKeyAliasesFollowPrimitiveNativeOracle(){
        Fixture f;
        for(const QByteArray bytes:{QByteArray(" \xef\xbb\xbfplaySound=true\n"),QByteArray("\xef\xbb\xbfplaySound=true\n")}){
            const auto oracleName=f.p("/primitive-oracle.ini");QVERIFY(f.put("/primitive-oracle.ini",bytes));
            {QSettings oracle(oracleName,QSettings::IniFormat);oracle.setFallbacksEnabled(false);QCOMPARE(oracle.allKeys(),QStringList{QStringLiteral("playSound")});QVERIFY(oracle.value("playSound").toBool());}
            QVERIFY(f.put(User,bytes));const auto baseline=f.snapshot();auto draft=baseline;QString error;
            QVERIFY(AeroLoginTypes::stage(draft,baseline,AeroLoginKey::StartupSound,false,error));const auto result=apply(f.c,baseline,draft);
            QVERIFY2(result.success,qPrintable(result.error));QVERIFY(!f.get(User).contains("\xef\xbb\xbf"));QVERIFY(!result.snapshot.fields[AeroLoginKey::StartupSound].value);
        }
    }
    void opaqueForeignPrefixesNeverInvokeRegisteredVariantDecoder(){
        Fixture f;qRegisterMetaType<AeroLoginDecoderCanary>();const auto encodedName=f.p("/encoded-canary.ini");
        {QSettings encoded(encodedName,QSettings::IniFormat);encoded.setValue("value",QVariant::fromValue(AeroLoginDecoderCanary{}));encoded.sync();QCOMPARE(encoded.status(),QSettings::NoError);}
        QFile encodedFile(encodedName);QVERIFY(encodedFile.open(QIODevice::ReadOnly));const auto encoded=encodedFile.readAll();const auto begin=encoded.indexOf("value=");QVERIFY(begin>=0);
        const auto rhs=encoded.mid(begin+6).trimmed();QVERIFY(rhs.contains("@Variant("));
        QVERIFY(f.put("/canary-oracle.ini",encoded));canaryDecodes=0;
        {QSettings oracle(f.p("/canary-oracle.ini"),QSettings::IniFormat);QCOMPARE(oracle.value("value").value<AeroLoginDecoderCanary>().value,7);}
        QVERIFY(canaryDecodes>0);canaryDecodes=0;
        const auto foreign="#foreign="+rhs+'\n'+"\v[foreign]="+rhs+'\n'+"\f[foreign]="+rhs+'\n';
        QVERIFY(f.put(User,"[General]\n"+foreign+"playSound=true\n"));const auto baseline=f.snapshot();QVERIFY(baseline.sourceValid);QCOMPARE(canaryDecodes,0);
        auto draft=baseline;QString error;QVERIFY(AeroLoginTypes::stage(draft,baseline,AeroLoginKey::StartupSound,false,error));const auto result=apply(f.c,baseline,draft);
        QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(canaryDecodes,0);QVERIFY(f.get(User).contains(foreign));
    }
    void defaultsRemoveAllOwnedAliasesWithoutFlatteningForeign(){
        Fixture f;QVERIFY(f.put(User,"[General]\n%65nableStartup=true\nenableStartup=true\nforeign=keep\n[Other]\nenableStartup=unowned\n"));
        const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::restore(d,b,AeroLoginKey::StartupAnimation,error));const auto result=apply(f.c,b,d);
        QVERIFY2(result.success,qPrintable(result.error));QVERIFY(!result.snapshot.fields[AeroLoginKey::StartupAnimation].localPresent);QVERIFY(!result.snapshot.fields[AeroLoginKey::StartupAnimation].value);
        QVERIFY(f.get(User).contains("foreign=keep"));QVERIFY(f.get(User).contains("[Other]\nenableStartup=unowned"));QVERIFY(!f.get(User).contains("%65nableStartup"));
    }
    void imageReplacementUsesIndependentValidatorAndKeepsApprovedDefault(){
        Fixture f;const auto defaultBytes=f.get(Default);const auto b=f.snapshot();auto d=b;QString error;const auto picture=f.picture();QVERIFY(picture.valid);QVERIFY(AeroLoginTypes::selectImage(d,b,picture,error));
        const auto result=apply(f.c,b,d);QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(f.validations,1);QCOMPARE(f.get(Image),picture.bytes);QCOMPARE(f.get(Default),defaultBytes);QCOMPARE(result.snapshot.background.digest,picture.digest);
    }
    void noValidatorCannotWriteImageAndRejectionHasNoJournal(){
        Fixture f;const auto old=f.get(Image);const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::selectImage(d,b,f.picture(),error));
        f.c.validateImage={};auto result=apply(f.c,b,d);QVERIFY(!result.success);QCOMPARE(f.get(Image),old);QVERIFY(!QFileInfo::exists(f.p(Journal)));
        f.c.validateImage=[](const QByteArray &,QString &e){e="Rejected by independent decoder";return false;};result=apply(f.c,b,d);QVERIFY(!result.success);QVERIFY(result.error.contains("Rejected"));QCOMPARE(f.get(Image),old);
    }
    void removeIdempotentAndRestoreCopiesExactDefaultBytes(){
        Fixture f(false);auto b=f.snapshot(),d=b;QString error;QVERIFY(AeroLoginTypes::removeImage(d,b,error));auto result=apply(f.c,b,d);QVERIFY2(result.success,qPrintable(result.error));QVERIFY(!QFileInfo::exists(f.p(Image)));QVERIFY(result.snapshot.fields[AeroLoginKey::RdpBackground].value);
        b=f.snapshot();d=b;QVERIFY(AeroLoginTypes::restoreImage(d,b,error));result=apply(f.c,b,d);QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(f.get(Image),f.get(Default));QVERIFY(!result.snapshot.fields[AeroLoginKey::RdpBackground].value);QCOMPARE(f.validations,1);
    }
    void staleRevisionAndChangedStaticSourceRefuseWithoutMutations(){
        Fixture f;const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));QVERIFY(f.put(User,"[Foreign]\nkeep=outside\n"));const auto bytes=f.get(User);
        auto result=apply(f.c,b,d);QVERIFY(!result.success);QCOMPARE(f.get(User),bytes);QVERIFY(!QFileInfo::exists(f.p(Journal)));
        QVERIFY(f.put(Main,"unapproved changed source"));const auto s=f.snapshot();QVERIFY(!s.sourceValid);QVERIFY(!s.writable);result=apply(f.c,s,d);QVERIFY(!result.success);QCOMPARE(f.get(User),bytes);
    }
    void unsafePermissionsSymlinksFifosAndAliasesRemainReadOnly(){
        Fixture f;QVERIFY(f.put(User,"[General]\nplaySound=true\n"));QVERIFY(::chmod(QFile::encodeName(f.p(User)).constData(),0666)==0);QVERIFY(!f.snapshot().trustedInstallation);QVERIFY(::chmod(QFile::encodeName(f.p(User)).constData(),0644)==0);
        QVERIFY(QFile::remove(f.p(User)));QVERIFY(QFile::link(f.p(Theme+"/theme.conf"),f.p(User)));QVERIFY(!f.snapshot().trustedInstallation);
        QVERIFY(QFile::remove(f.p(User)));QVERIFY(::mkfifo(QFile::encodeName(f.p(User)).constData(),0600)==0);QVERIFY(!f.snapshot().sourceValid);
        QVERIFY(QFile::remove(f.p(User)));QVERIFY(::link(QFile::encodeName(f.p(Default)).constData(),QFile::encodeName(f.p(User)).constData())==0);QVERIFY(!f.snapshot().trustedInstallation);
    }
    void privateOriginalsAreNeverExposedInPublicJournal(){
        Fixture f;QVERIFY(f.put(User,"[General]\nplaySound=true\nprivateForeign=secret\n"));QVERIFY(::chmod(QFile::encodeName(f.p(User)).constData(),0600)==0);const auto s=f.snapshot();QVERIFY(!s.trustedInstallation&&!s.writable);QVERIFY(!QFileInfo::exists(f.p(Journal)));
        QVERIFY(::chmod(QFile::encodeName(f.p(User)).constData(),0644)==0);QVERIFY(::chmod(QFile::encodeName(f.p(Default)).constData(),0600)==0);QVERIFY(!f.snapshot().trustedInstallation);QVERIFY(!QFileInfo::exists(f.p(Journal)));
    }
    void opaqueOwnedValueIsUnknownButCanBeExplicitlyRepaired(){
        Fixture f;QVERIFY(f.put(User,"[General]\nplaySound=@Variant(serialized QImage payload not decoded)\n"));const auto b=f.snapshot();QVERIFY(b.sourceValid);QVERIFY(!b.fields[AeroLoginKey::StartupSound].known);auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));const auto r=apply(f.c,b,d);QVERIFY2(r.success,qPrintable(r.error));QVERIFY(r.snapshot.fields[AeroLoginKey::StartupSound].known);QVERIFY(!r.snapshot.fields[AeroLoginKey::StartupSound].value);
    }
    void stageAndCommitFailuresRollbackBothFiles_data(){
        QTest::addColumn<int>("point");for(auto point:{FailurePoint::StageConfig,FailurePoint::StageImage,FailurePoint::CommitConfig,FailurePoint::CommitImage,FailurePoint::Readback})QTest::newRow(qPrintable(QString::number(int(point))))<<int(point);
    }
    void stageAndCommitFailuresRollbackBothFiles(){
        QFETCH(int,point);Fixture f;const auto oldImage=f.get(Image);const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));QVERIFY(AeroLoginTypes::selectImage(d,b,f.picture(),error));
        f.c.fail=[point](FailurePoint p){return int(p)==point;};const auto r=apply(f.c,b,d);QVERIFY(!r.success);QVERIFY2(r.rolledBack,qPrintable(r.error));QVERIFY(!r.recoveryRequired);QCOMPARE(f.get(Image),oldImage);QVERIFY(!QFileInfo::exists(f.p(User)));QVERIFY(!QFileInfo::exists(f.p(Journal)));
    }
    void stagedForeignStateIsNeverInstalled_data(){
        QTest::addColumn<bool>("config");QTest::addColumn<int>("mutation");
        for(bool config:{true,false})for(int mutation=0;mutation<4;++mutation)
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(config?"config":"image").arg(mutation)))<<config<<mutation;
    }
    void stagedForeignStateIsNeverInstalled(){
        QFETCH(bool,config);QFETCH(int,mutation);Fixture f;QVERIFY(f.put(User,"[General]\nplaySound=true\n"));
        const auto oldUser=f.get(User),oldImage=f.get(Image);const auto b=f.snapshot();auto d=b;QString error;
        QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));QVERIFY(AeroLoginTypes::selectImage(d,b,f.picture(),error));
        const auto stage=Theme+(config?"/.controlpanel-aero-login.stage.config":"/.controlpanel-aero-login.stage.image");bool mutated=false,mutationOk=false;
        f.c.fail=[&](FailurePoint p){
            if(p!=(config?FailurePoint::CommitConfig:FailurePoint::CommitImage)||mutated)return false;
            mutated=true;
            if(mutation==0)mutationOk=f.put(stage,"foreign staged bytes");
            else if(mutation==1)mutationOk=::chmod(QFile::encodeName(f.p(stage)).constData(),0600)==0;
            else if(mutation==2)mutationOk=QFile::remove(f.p(stage))&&QFile::link(f.p(Default),f.p(stage));
            else {const auto alternate=stage+".replacement";mutationOk=f.put(alternate,f.get(stage))&&::rename(QFile::encodeName(f.p(alternate)).constData(),QFile::encodeName(f.p(stage)).constData())==0;}
            return false;
        };
        const auto r=apply(f.c,b,d);QVERIFY(mutated&&mutationOk);QVERIFY(!r.success);QVERIFY(!r.error.isEmpty());
        QCOMPARE(f.get(config?User:Image),config?oldUser:oldImage);
        if(mutation<3){QVERIFY(r.recoveryRequired);QVERIFY(QFileInfo::exists(f.p(Journal)));QVERIFY(QFileInfo(f.p(stage)).exists());}
        else {QVERIFY(r.rolledBack);QVERIFY(!r.recoveryRequired);QCOMPARE(f.get(User),oldUser);QCOMPARE(f.get(Image),oldImage);}
    }
    void journalDirectorySyncFailureReportsActualRecoverySnapshot(){
        Fixture f;const auto oldImage=f.get(Image);const auto b=f.snapshot();auto d=b;QString error;
        QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));
        f.c.fail=[](FailurePoint p){return p==FailurePoint::JournalSync;};const auto r=apply(f.c,b,d);
        QVERIFY(!r.success&&!r.saved);QVERIFY(r.recoveryRequired);QVERIFY(r.snapshot.recoveryPending&&r.snapshot.canRecover);
        QCOMPARE(r.snapshot.revision,f.snapshot().revision);QVERIFY(!QFileInfo::exists(f.p(User)));QCOMPARE(f.get(Image),oldImage);
        f.c.fail={};const auto recovered=recover(f.c,r.snapshot.revision);QVERIFY2(recovered.success,qPrintable(recovered.error));QVERIFY(!f.snapshot().recoveryPending);
    }
    void interruptedCommitRecoveryIsExplicitAndReturnsToBefore_data(){
        QTest::addColumn<int>("point");QTest::newRow("first")<<int(FailurePoint::CrashAfterFirstCommit);QTest::newRow("second")<<int(FailurePoint::CrashAfterSecondCommit);
    }
    void interruptedCommitRecoveryIsExplicitAndReturnsToBefore(){
        QFETCH(int,point);Fixture f;const auto oldImage=f.get(Image);const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));QVERIFY(AeroLoginTypes::selectImage(d,b,f.picture(),error));f.c.fail=[point](FailurePoint p){return int(p)==point;};
        auto r=apply(f.c,b,d);QVERIFY(r.recoveryRequired);const auto beforeRead=f.get(User);const auto pending=f.snapshot();QVERIFY(pending.recoveryPending&&pending.canRecover);QCOMPARE(f.get(User),beforeRead);QVERIFY(QFileInfo::exists(f.p(Journal)));
        for(const auto key:AeroLoginTypes::keys())QVERIFY(pending.recoverySummary.contains(AeroLoginTypes::keyName(key)+": "));
        QVERIFY(pending.recoverySummary.contains("playSound: false → true"));QVERIFY(pending.recoverySummary.contains("saved override absent"));
        QVERIFY(pending.recoverySummary.contains(QString::fromLatin1(digest(oldImage).toHex())));QVERIFY(pending.recoverySummary.contains("Background current:"));QVERIFY(pending.recoverySummary.contains("Background restored:"));
        QVERIFY(pending.recoverySummary.contains("saved file bytes only"));QVERIFY(pending.recoverySummary.contains("adoption are not verified"));
        f.c.fail={};r=recover(f.c,pending.revision);QVERIFY2(r.success,qPrintable(r.error));QVERIFY(r.rolledBack);QCOMPARE(f.get(Image),oldImage);QVERIFY(!QFileInfo::exists(f.p(User)));QVERIFY(!f.snapshot().recoveryPending);
    }
    void recoveryReviewDistinguishesOpaqueAndAbsentBackground(){
        Fixture f(false);QVERIFY(f.put(User,"[General]\nplaySound=@Variant(private opaque spelling)\n"));const auto b=f.snapshot();auto d=b;QString error;
        QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));f.c.fail=[](FailurePoint p){return p==FailurePoint::CrashAfterFirstCommit;};
        const auto result=apply(f.c,b,d);QVERIFY(result.recoveryRequired);const auto pending=f.snapshot();QVERIFY(pending.canRecover);
        QVERIFY(pending.recoverySummary.contains("playSound: false → unknown"));QVERIFY(pending.recoverySummary.contains("saved override opaque; unverified"));
        QVERIFY(!pending.recoverySummary.contains("private opaque spelling"));QVERIFY(pending.recoverySummary.contains("Background restored: absent"));QVERIFY(pending.recoverySummary.contains("blue fallback"));QVERIFY(pending.recoverySummary.contains("approved default background"));
        f.c.fail={};QVERIFY(recover(f.c,pending.revision).success);
    }
    void malformedSavedConfigurationBlocksRecoveryReview(){
        Fixture f;QVERIFY(f.put(User,"[General]\nplaySound=true\n"));const auto b=f.snapshot();auto d=b;QString error;
        QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));f.c.fail=[](FailurePoint p){return p==FailurePoint::CrashAfterFirstCommit;};QVERIFY(apply(f.c,b,d).recoveryRequired);
        auto record=QJsonDocument::fromJson(f.get(Journal)).object();auto before=record["beforeUser"].toObject();const QByteArray malformed="[unterminated section";
        before["bytes"]=QString::fromLatin1(malformed.toBase64());before["digest"]=QString::fromLatin1(digest(malformed).toHex());record["beforeUser"]=before;QVERIFY(f.put(Journal,QJsonDocument(record).toJson(QJsonDocument::Compact)));
        const auto pending=f.snapshot();QVERIFY(pending.recoveryPending&&!pending.canRecover);QVERIFY(pending.recoverySummary.contains("INI is malformed"));f.c.fail={};QVERIFY(!recover(f.c,pending.revision).success);QVERIFY(QFileInfo::exists(f.p(Journal)));
    }
    void failedRollbackLeavesRecoverableJournal(){
        Fixture f;const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));f.c.fail=[](FailurePoint p){return p==FailurePoint::Readback||p==FailurePoint::Rollback;};
        const auto r=apply(f.c,b,d);QVERIFY(!r.success);QVERIFY(r.recoveryRequired);const auto pending=f.snapshot();QVERIFY(pending.canRecover);f.c.fail={};QVERIFY(recover(f.c,pending.revision).success);QVERIFY(!QFileInfo::exists(f.p(User)));
    }
    void partialRollbackRetainsARecoverableKnownPrefix(){
        Fixture f;const auto old=f.get(Image);const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));QVERIFY(AeroLoginTypes::selectImage(d,b,f.picture(),error));
        f.c.fail=[](FailurePoint p){return p==FailurePoint::Readback||p==FailurePoint::RollbackImage;};const auto r=apply(f.c,b,d);QVERIFY(r.recoveryRequired);QVERIFY(!QFileInfo::exists(f.p(User)));QVERIFY(f.get(Image)!=old);const auto pending=f.snapshot();QVERIFY(pending.canRecover);
        f.c.fail={};const auto recovered=recover(f.c,pending.revision);QVERIFY2(recovered.success,qPrintable(recovered.error));QCOMPARE(f.get(Image),old);
    }
    void foreignEditBetweenRollbackFilesIsNeverOverwritten(){
        Fixture f;const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));QVERIFY(AeroLoginTypes::selectImage(d,b,f.picture(),error));
        f.c.fail=[&](FailurePoint p){if(p==FailurePoint::Readback)return true;if(p==FailurePoint::RollbackImage)f.put(User,"[Foreign]\nkeep=rollback-race\n");return false;};
        const auto r=apply(f.c,b,d);QVERIFY(r.recoveryRequired);QVERIFY(f.get(User).contains("rollback-race"));QVERIFY(!f.snapshot().canRecover);QVERIFY(QFileInfo::exists(f.p(Journal)));
    }
    void foreignEditAndStaleRecoveryNeverOverwriteOutsideChanges(){
        Fixture f;const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));f.c.fail=[](FailurePoint p){return p==FailurePoint::CrashAfterFirstCommit;};QVERIFY(apply(f.c,b,d).recoveryRequired);const auto pending=f.snapshot();
        QVERIFY(f.put(User,"[Foreign]\nkeep=external\n"));const auto outside=f.get(User);QVERIFY(!f.snapshot().canRecover);f.c.fail={};QVERIFY(!recover(f.c,pending.revision).success);QCOMPARE(f.get(User),outside);QVERIFY(QFileInfo::exists(f.p(Journal)));
    }
    void lockContentionAndOrphanStageNeverStartAnotherTransaction(){
        Fixture f;const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));
        const auto lockName=QFile::encodeName(f.p(Theme+"/.controlpanel-aero-login.lock"));const int fd=::open(lockName.constData(),O_CREAT|O_RDWR,0600);QVERIFY(fd>=0);QVERIFY(::flock(fd,LOCK_EX|LOCK_NB)==0);
        const auto blocked=apply(f.c,b,d);QVERIFY(!blocked.success);QVERIFY(!QFileInfo::exists(f.p(User)));::flock(fd,LOCK_UN);::close(fd);
        QVERIFY(f.put(Theme+"/.controlpanel-aero-login.stage.config","unknown stage"));const auto orphan=f.snapshot();QVERIFY(orphan.recoveryPending&&!orphan.canRecover&&!orphan.writable);QVERIFY(!apply(f.c,orphan,d).success);QVERIFY(f.get(Theme+"/.controlpanel-aero-login.stage.config")==QByteArray("unknown stage"));
    }
    void journalAndStagingEditsAreNeverDiscardedAsOurCleanup(){
        Fixture f;const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));
        f.c.fail=[&](FailurePoint p){if(p==FailurePoint::Readback){f.put(Journal,"foreign journal edit");return true;}return false;};
        const auto r=apply(f.c,b,d);QVERIFY(r.recoveryRequired);QCOMPARE(f.get(Journal),QByteArray("foreign journal edit"));QVERIFY(!f.snapshot().canRecover);QVERIFY(QFileInfo::exists(f.p(User)));
    }
    void unicodeKeyAliasesAndForeignGeneralSectionRemainDistinct(){
        Fixture f;QVERIFY(f.put(User,"[General]\n%U0070laySound=true\n[%%General]\nplaySound=foreign-double-percent\n[%General]\nplaySound=foreign-rootname\n"));const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));
        const auto r=apply(f.c,b,d);QVERIFY2(r.success,qPrintable(r.error));QVERIFY(!f.get(User).contains("%U0070laySound"));QVERIFY(f.get(User).contains("[%%General]\nplaySound=foreign-double-percent"));QVERIFY(f.get(User).contains("[%General]\nplaySound=foreign-rootname"));
    }
    void validationRaceAndUntrustedContextCannotChooseOtherTargets(){
        Fixture f;const auto b=f.snapshot();auto d=b;QString error;QVERIFY(AeroLoginTypes::selectImage(d,b,f.picture(),error));f.c.validateImage=[&](const QByteArray &,QString &){f.put(User,"[Foreign]\nkeep=validation-race\n");return true;};
        QVERIFY(!apply(f.c,b,d).success);QVERIFY(f.get(User).contains("validation-race"));QVERIFY(!QFileInfo::exists(f.p(Journal)));
        auto invalid=f.c;invalid.fixture=false;QVERIFY(!read(invalid).sourceValid);QVERIFY(!apply(invalid,b,d).success);
    }
};
QTEST_GUILESS_MAIN(AeroLoginFilesTest)
#include "aero_login_files_test.moc"
