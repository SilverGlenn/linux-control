#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include "login/AeroLoginProtocol.h"
#include "aero_login_fixture.h"

namespace {
QByteArray packed(const QJsonObject &o){return QJsonDocument(o).toJson(QJsonDocument::Compact);}
QJsonObject request(){auto b=aeroLoginFixture(),d=b;QString error;AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error);return QJsonDocument::fromJson(AeroLoginProtocol::encode(b,d,error)).object();}
}
class AeroLoginProtocolTest final:public QObject {
    Q_OBJECT
private slots:
    void explicitRequestsRoundTripWithoutCallerPaths(){
        const auto b=aeroLoginFixture();
        for(int operation=0;operation<5;++operation){
            auto d=b;QString error;
            if(operation==0)QVERIFY(AeroLoginTypes::stage(d,b,AeroLoginKey::StartupSound,false,error));
            if(operation==1)QVERIFY(AeroLoginTypes::selectImage(d,b,aeroLoginImageFixture(Qt::red),error));
            if(operation==2)QVERIFY(AeroLoginTypes::removeImage(d,b,error));
            if(operation==3)QVERIFY(AeroLoginTypes::restoreImage(d,b,error));
            if(operation==4){auto local=b;local.fields[AeroLoginKey::StartupAnimation].localPresent=true;local.fields[AeroLoginKey::StartupAnimation].raw="true";local.fields[AeroLoginKey::StartupAnimation].value=true;d=local;QVERIFY(AeroLoginTypes::restore(d,local,AeroLoginKey::StartupAnimation,error));auto bytes=AeroLoginProtocol::encode(local,d,error);AeroLoginSnapshot decoded;bool recovery;QVERIFY2(AeroLoginProtocol::decodeRequest(bytes,local,decoded,recovery,error),qPrintable(error));QVERIFY(AeroLoginTypes::sameDraft(d,decoded));continue;}
            const auto bytes=AeroLoginProtocol::encode(b,d,error);QVERIFY2(!bytes.isEmpty(),qPrintable(error));QVERIFY(!bytes.contains("/usr/")&&!bytes.contains("/home/"));
            AeroLoginSnapshot decoded;bool recovery=true;QVERIFY2(AeroLoginProtocol::decodeRequest(bytes,b,decoded,recovery,error),qPrintable(error));QVERIFY(!recovery);QVERIFY(AeroLoginTypes::sameDraft(d,decoded));
        }
    }
    void schemasRejectUnknownKeysTypesModesAndRevisions(){
        const auto b=aeroLoginFixture();const auto good=request();QString error;AeroLoginSnapshot d;bool recover=false;
        QList<QJsonObject> bad;
        auto o=good;o["path"]="/etc/shadow";bad<<o;o=good;o["version"]="1";bad<<o;o=good;o["version"]=1.5;bad<<o;o=good;o["mode"]="write-file";bad<<o;
        o=good;o["revision"]=QString(64,'0');bad<<o;o=good;o["revision"]=QString::fromLatin1(b.revision.toHex()).toUpper();bad<<o;
        o=good;o["fields"]=QJsonObject{{"foreign",QJsonObject{{"intent","set"},{"value",true}}}};bad<<o;
        for(auto value:{QJsonValue(1),QJsonValue("false"),QJsonValue(),QJsonValue(QJsonObject{})}){o=good;o["fields"]=QJsonObject{{"playSound",QJsonObject{{"intent","set"},{"value",value}}}};bad<<o;}
        o=good;o["fields"]=QJsonObject{{"playSound",QJsonObject{{"intent","default"},{"value",false}}}};bad<<o;
        o=good;o["background"]="theme";bad<<o;o=good;o["image"]=QJsonObject{{"path","/private/picture"}};bad<<o;
        o=good;o["fields"]=QJsonObject{};bad<<o;
        for(const auto &value:bad){QVERIFY2(!AeroLoginProtocol::decodeRequest(packed(value),b,d,recover,error),packed(value).constData());QVERIFY(!error.isEmpty());}
    }
    void duplicateKeysIncludingEscapedSpellingAndNestedKeysAreRejected(){
        const auto b=aeroLoginFixture();const auto good=packed(request());QString error;AeroLoginSnapshot d;bool recover=false;
        auto duplicate=good;duplicate.insert(1,"\"version\":1,");QVERIFY(!AeroLoginProtocol::decodeRequest(duplicate,b,d,recover,error));
        duplicate=good;duplicate.insert(1,"\"vers\\u0069on\":1,");QVERIFY(!AeroLoginProtocol::decodeRequest(duplicate,b,d,recover,error));
        duplicate=good;duplicate.replace("\"intent\":\"set\"","\"intent\":\"set\",\"in\\u0074ent\":\"set\"");QVERIFY(!AeroLoginProtocol::decodeRequest(duplicate,b,d,recover,error));
        duplicate=QByteArray(20,'[')+good+QByteArray(20,']');QVERIFY(!AeroLoginProtocol::decodeRequest(duplicate,b,d,recover,error));
        QVERIFY(!AeroLoginProtocol::decodeRequest(good+good,b,d,recover,error));
        QVERIFY(!AeroLoginProtocol::decodeRequest(QByteArray(AeroLoginProtocol::MaximumRequestBytes+1,' '),b,d,recover,error));
    }
    void contradictoryBackgroundCompanionsCannotBeSilentlyRewritten(){
        const auto b=aeroLoginFixture();auto o=request();QString error;AeroLoginSnapshot d;bool recovery=false;
        o["fields"]=QJsonObject{{"rdpBackground",QJsonObject{{"intent","set"},{"value",true}}}};
        QVERIFY(!AeroLoginProtocol::decodeRequest(packed(o),b,d,recovery,error));
        auto staged=b;QVERIFY(AeroLoginTypes::selectImage(staged,b,aeroLoginImageFixture(),error));o=QJsonDocument::fromJson(AeroLoginProtocol::encode(b,staged,error)).object();o["fields"]=QJsonObject{{"rdpBackground",QJsonObject{{"intent","set"},{"value",true}}}};
        QVERIFY(!AeroLoginProtocol::decodeRequest(packed(o),b,d,recovery,error));
    }
    void malformedImageMetadataDigestAndBase64AreRejected(){
        const auto b=aeroLoginFixture();auto d=b;QString error;QVERIFY(AeroLoginTypes::selectImage(d,b,aeroLoginImageFixture(),error));const auto good=QJsonDocument::fromJson(AeroLoginProtocol::encode(b,d,error)).object();
        QList<QJsonObject> images;auto i=good["image"].toObject();i["width"]=0;images<<i;i=good["image"].toObject();i["height"]=1.5;images<<i;i=good["image"].toObject();i["width"]=100000;images<<i;i=good["image"].toObject();i["format"]="jpeg";images<<i;
        i=good["image"].toObject();i["data"]=i["data"].toString()+"\n";images<<i;i=good["image"].toObject();i["sha256"]=QString(64,'0');images<<i;i=good["image"].toObject();i["file"]="/etc/shadow";images<<i;
        bool recover=false;for(const auto &image:images){auto o=good;o["image"]=image;QVERIFY(!AeroLoginProtocol::decodeRequest(packed(o),b,d,recover,error));QVERIFY(!error.isEmpty());}
    }
    void recoveryIsSeparateFreshAndContainsNoEdits(){
        auto b=aeroLoginFixture();QString error;QVERIFY(AeroLoginProtocol::encodeRecovery(b,error).isEmpty());b.recoveryPending=b.canRecover=true;b.writable=false;for(auto &field:b.fields)field.writable=false;
        const auto bytes=AeroLoginProtocol::encodeRecovery(b,error);QVERIFY2(!bytes.isEmpty(),qPrintable(error));AeroLoginSnapshot d;bool recover=false;QVERIFY(AeroLoginProtocol::decodeRequest(bytes,b,d,recover,error));QVERIFY(recover);
        auto o=QJsonDocument::fromJson(bytes).object();o["fields"]=request()["fields"];QVERIFY(!AeroLoginProtocol::decodeRequest(packed(o),b,d,recover,error));o=QJsonDocument::fromJson(bytes).object();o["background"]="remove";QVERIFY(!AeroLoginProtocol::decodeRequest(packed(o),b,d,recover,error));
        b.canRecover=false;QVERIFY(!AeroLoginProtocol::decodeRequest(bytes,b,d,recover,error));
    }
    void savedRepliesRequireExactReceiptAndStrictSchema(){
        const auto revision=aeroLoginFixture().revision;bool saved=false,pending=false;QString error;QByteArray receipt;
        QVERIFY(AeroLoginProtocol::decodeReply(AeroLoginProtocol::encodeReply(true,{},false,revision),saved,error,pending,&receipt));QVERIFY(saved&&!pending);QCOMPARE(receipt,revision);
        QVERIFY(!AeroLoginProtocol::decodeReply(AeroLoginProtocol::encodeReply(true,{},false),saved,error,pending,&receipt));
        QVERIFY(AeroLoginProtocol::decodeReply(AeroLoginProtocol::encodeReply(false,"Denied",false),saved,error,pending,&receipt));QVERIFY(!saved);QCOMPARE(error,QStringLiteral("Denied"));QVERIFY(receipt.isEmpty());
        auto o=QJsonDocument::fromJson(AeroLoginProtocol::encodeReply(true,{},false,revision)).object();o["saved"]="true";QVERIFY(!AeroLoginProtocol::decodeReply(packed(o),saved,error,pending));o["saved"]=true;o["future"]=true;QVERIFY(!AeroLoginProtocol::decodeReply(packed(o),saved,error,pending));
        auto duplicate=AeroLoginProtocol::encodeReply(true,{},false,revision);duplicate.insert(1,"\"saved\":true,");QVERIFY(!AeroLoginProtocol::decodeReply(duplicate,saved,error,pending));
    }
};
QTEST_GUILESS_MAIN(AeroLoginProtocolTest)
#include "aero_login_protocol_test.moc"
