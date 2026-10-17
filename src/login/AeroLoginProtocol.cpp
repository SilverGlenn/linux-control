#include "AeroLoginProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cmath>
#include <algorithm>
namespace {
// QJsonDocument keeps the last duplicate key. Reject duplicates independently,
// including escaped spellings, before interpreting an authorization request.
class UniqueKeys {
public:
    explicit UniqueKeys(const QByteArray &bytes):b(bytes){}
    bool check(){return value(0)&&(space(),i==b.size());}
private:
    const QByteArray &b;qsizetype i=0;
    void space(){while(i<b.size()&&(b[i]==' '||b[i]=='\n'||b[i]=='\r'||b[i]=='\t'))++i;}
    bool string(QString *key=nullptr){
        if(i>=b.size()||b[i]!='"')return false;const auto start=i++;
        while(i<b.size()){const char c=b[i++];if(c=='\\'){if(i>=b.size())return false;++i;}else if(c=='"'){
            if(key){if(i-start>512)return false;const auto doc=QJsonDocument::fromJson('['+b.mid(start,i-start)+']');if(!doc.isArray()||doc.array().size()!=1)return false;*key=doc.array()[0].toString();}return true;}}
        return false;
    }
    bool value(int depth){
        space();if(depth>12||i>=b.size())return false;
        if(b[i]=='{'){++i;space();QSet<QString> keys;if(i<b.size()&&b[i]=='}'){++i;return true;}
            for(;;){QString key;if(!string(&key)||keys.contains(key))return false;keys.insert(key);space();if(i>=b.size()||b[i++]!=':'||!value(depth+1))return false;space();if(i>=b.size())return false;if(b[i]=='}'){++i;return true;}if(b[i++]!=',')return false;space();}}
        if(b[i]=='['){++i;space();if(i<b.size()&&b[i]==']'){++i;return true;}for(;;){if(!value(depth+1))return false;space();if(i>=b.size())return false;if(b[i]==']'){++i;return true;}if(b[i++]!=',')return false;}}
        if(b[i]=='"')return string();const auto start=i;while(i<b.size()&&b[i]!=','&&b[i]!='}'&&b[i]!=']'&&b[i]!=' '&&b[i]!='\n'&&b[i]!='\r'&&b[i]!='\t')++i;return i>start;
    }
};
bool objectKeys(const QJsonObject &o,const QStringList &keys){auto actual=o.keys(),expected=keys;actual.sort();expected.sort();return actual==expected;}
bool hex(const QString &s,QByteArray &out,bool empty=false){if(empty&&s.isEmpty()){out.clear();return true;}if(s.size()!=64)return false;for(auto c:s)if(!(c>='0'&&c<='9'||c>='a'&&c<='f'))return false;out=QByteArray::fromHex(s.toLatin1());return out.size()==32;}
QJsonObject parse(const QByteArray &bytes,qsizetype limit,QString &error)
{
    if(bytes.isEmpty()||bytes.size()>limit||!UniqueKeys(bytes).check()){error=QStringLiteral("The bounded appearance protocol contains invalid or duplicate JSON keys.");return {};}
    QJsonParseError e;auto doc=QJsonDocument::fromJson(bytes,&e);if(e.error!=QJsonParseError::NoError||!doc.isObject()){error=QStringLiteral("The appearance protocol is not a JSON object.");return {};}
    return doc.object();
}
QString backgroundName(AeroLoginBackgroundIntent intent){switch(intent){case AeroLoginBackgroundIntent::Keep:return QStringLiteral("keep");case AeroLoginBackgroundIntent::ReplaceImage:return QStringLiteral("replace");case AeroLoginBackgroundIntent::RemoveOverride:return QStringLiteral("remove");case AeroLoginBackgroundIntent::RestoreApprovedImage:return QStringLiteral("approved");}return {};}
bool integer(const QJsonValue &v,int &out){if(!v.isDouble()||!std::isfinite(v.toDouble())||v.toDouble()<1||v.toDouble()>2147483647||std::floor(v.toDouble())!=v.toDouble())return false;out=v.toInt();return true;}
}
QByteArray AeroLoginProtocol::encode(const AeroLoginSnapshot &expected,const AeroLoginSnapshot &requested,QString &error)
{
    error.clear();if(expected.revision.size()!=32||!AeroLoginTypes::dirty(requested)||!AeroLoginTypes::requestValid(expected,requested,error)){if(error.isEmpty())error=QStringLiteral("An explicit appearance request and a fresh source revision are required.");return {};}
    QJsonObject fields;for(auto it=requested.intents.cbegin();it!=requested.intents.cend();++it){QJsonObject f{{"intent",it.value()==AeroLoginIntent::Set?"set":"default"}};if(it.value()==AeroLoginIntent::Set)f.insert("value",requested.fields[it.key()].value);fields.insert(AeroLoginTypes::keyName(it.key()),f);}
    QJsonValue image;
    if(requested.backgroundIntent==AeroLoginBackgroundIntent::ReplaceImage){const auto &i=requested.replacement;image=QJsonObject{{"data",QString::fromLatin1(i.bytes.toBase64())},{"sha256",QString::fromLatin1(i.digest.toHex())},{"width",i.width},{"height",i.height},{"format",i.format}};}
    const auto bytes=QJsonDocument(QJsonObject{{"version",Version},{"mode","apply"},{"revision",QString::fromLatin1(expected.revision.toHex())},{"fields",fields},{"background",backgroundName(requested.backgroundIntent)},{"image",image}}).toJson(QJsonDocument::Compact);
    if(bytes.size()>MaximumRequestBytes){error=QStringLiteral("The prepared appearance request exceeds the transport limit.");return {};}return bytes;
}
QByteArray AeroLoginProtocol::encodeRecovery(const AeroLoginSnapshot &expected,QString &error)
{
    error.clear();if(expected.revision.size()!=32||!expected.recoveryPending||!expected.canRecover||!expected.sourceValid||!expected.trustedInstallation){error=QStringLiteral("No safely recoverable appearance transaction is available.");return {};}
    return QJsonDocument(QJsonObject{{"version",Version},{"mode","recover"},{"revision",QString::fromLatin1(expected.revision.toHex())},{"fields",QJsonObject{}},{"background","keep"},{"image",QJsonValue()}}).toJson(QJsonDocument::Compact);
}
bool AeroLoginProtocol::decodeRequest(const QByteArray &bytes,const AeroLoginSnapshot &current,AeroLoginSnapshot &requested,bool &recovery,QString &error)
{
    error.clear();recovery=false;requested={};const auto o=parse(bytes,MaximumRequestBytes,error);if(!error.isEmpty())return false;
    QByteArray revision;if(!objectKeys(o,{"version","mode","revision","fields","background","image"})||!o["version"].isDouble()||o["version"].toDouble()!=Version||!o["mode"].isString()||!o["revision"].isString()||!hex(o["revision"].toString(),revision)||revision!=current.revision||!o["fields"].isObject()||!o["background"].isString()){error=QStringLiteral("The appearance request schema or source revision is unsupported.");return false;}
    requested=current;
    if(o["mode"].toString()==QLatin1String("recover")){
        if(!o["fields"].toObject().isEmpty()||o["background"].toString()!=QLatin1String("keep")||!o["image"].isNull()||!current.recoveryPending||!current.canRecover||!current.sourceValid||!current.trustedInstallation){error=QStringLiteral("The recovery request contains changes or an unsafe journal state.");return false;}recovery=true;return true;
    }
    if(o["mode"].toString()!=QLatin1String("apply")){error=QStringLiteral("Unknown appearance request mode.");return false;}
    const auto fields=o["fields"].toObject();for(auto it=fields.begin();it!=fields.end();++it){
        auto keys=AeroLoginTypes::keys();auto found=std::find_if(keys.begin(),keys.end(),[&](AeroLoginKey key){return AeroLoginTypes::keyName(key)==it.key();});
        if(found==keys.end()||!it.value().isObject()){error=QStringLiteral("An unowned appearance key was requested.");return false;}const auto key=*found;const auto f=it.value().toObject();
        if(f["intent"].toString()==QLatin1String("set")&&objectKeys(f,{"intent","value"})&&f["value"].isBool()){requested.fields[key].value=f["value"].toBool();requested.fields[key].known=true;requested.intents[key]=AeroLoginIntent::Set;}
        else if(f["intent"].toString()==QLatin1String("default")&&objectKeys(f,{"intent"})&&current.fields.value(key).defaultKnown){requested.fields[key].value=current.fields[key].defaultValue;requested.fields[key].known=true;requested.intents[key]=AeroLoginIntent::RestoreApprovedDefault;}
        else{error=QStringLiteral("The appearance key intent or boolean type is invalid.");return false;}
    }
    const auto name=o["background"].toString();bool found=false;for(auto intent:{AeroLoginBackgroundIntent::Keep,AeroLoginBackgroundIntent::ReplaceImage,AeroLoginBackgroundIntent::RemoveOverride,AeroLoginBackgroundIntent::RestoreApprovedImage})if(name==backgroundName(intent)){requested.backgroundIntent=intent;found=true;break;}
    if(!found){error=QStringLiteral("Unknown background intent.");return false;}
    if(requested.backgroundIntent==AeroLoginBackgroundIntent::ReplaceImage){
        if(!o["image"].isObject()){error=QStringLiteral("The prepared image is missing.");return false;}const auto i=o["image"].toObject();auto &image=requested.replacement;
        if(!objectKeys(i,{"data","sha256","width","height","format"})||!i["data"].isString()||!i["sha256"].isString()||!hex(i["sha256"].toString(),image.digest)||!integer(i["width"],image.width)||!integer(i["height"],image.height)||!i["format"].isString()||i["format"].toString()!=QLatin1String("png")){error=QStringLiteral("The prepared image metadata is invalid.");return false;}
        const auto encoded=i["data"].toString().toLatin1();const auto decoded=QByteArray::fromBase64Encoding(encoded,QByteArray::AbortOnBase64DecodingErrors);
        if(!decoded||decoded.decoded.isEmpty()||decoded.decoded.size()>AeroLoginTypes::MaximumImageBytes||decoded.decoded.toBase64()!=encoded){error=QStringLiteral("The prepared image data is invalid or oversized.");return false;}image.bytes=decoded.decoded;image.format=QStringLiteral("png");image.valid=true;
    }else if(!o["image"].isNull()){error=QStringLiteral("An image was supplied for an operation which does not replace it.");return false;}
    if(!AeroLoginTypes::dirty(requested)){error=QStringLiteral("An explicit appearance change is required.");return false;}
    return AeroLoginTypes::requestValid(current,requested,error);
}
QByteArray AeroLoginProtocol::encodeReply(bool saved,const QString &error,bool recoveryPending,const QByteArray &postRevision)
{return QJsonDocument(QJsonObject{{"version",Version},{"saved",saved},{"error",error.left(4096)},{"recoveryPending",recoveryPending},{"revision",QString::fromLatin1(postRevision.toHex())}}).toJson(QJsonDocument::Compact);}
bool AeroLoginProtocol::decodeReply(const QByteArray &bytes,bool &saved,QString &error,bool &recoveryPending,QByteArray *postRevision)
{
    saved=recoveryPending=false;error.clear();if(postRevision)postRevision->clear();const auto o=parse(bytes,MaximumReplyBytes,error);if(!error.isEmpty())return false;
    QByteArray revision;if(!objectKeys(o,{"version","saved","error","recoveryPending","revision"})||!o["version"].isDouble()||o["version"].toDouble()!=Version||!o["saved"].isBool()||!o["error"].isString()||o["error"].toString().size()>4096||!o["recoveryPending"].isBool()||!o["revision"].isString()||!hex(o["revision"].toString(),revision,!o["saved"].toBool())){error=QStringLiteral("The helper reply schema or saved revision is unsupported.");return false;}
    saved=o["saved"].toBool();recoveryPending=o["recoveryPending"].toBool();error=o["error"].toString();if(postRevision)*postRevision=revision;return true;
}
