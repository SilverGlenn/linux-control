#include "AeroLoginSettings.h"
#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QImage>
#include <QImageReader>
#include <QtEndian>
#include <cstring>

namespace {
QByteArray hash(const QByteArray &b){return QCryptographicHash::hash(b,QCryptographicHash::Sha256);}
bool editable(const AeroLoginSnapshot &s){return s.sourceValid&&s.trustedInstallation&&s.writable&&!s.recoveryPending;}
bool rawStage(AeroLoginSnapshot &d,const AeroLoginSnapshot &b,AeroLoginKey key,bool v,QString &error)
{
    if(!AeroLoginTypes::keys().contains(key)||!b.fields.contains(key)||!d.fields.contains(key)||!editable(b)||!b.fields[key].writable){error=QObject::tr("This approved Aero preference is not writable.");return false;}
    const auto &f=b.fields[key];if(f.known&&f.localPresent&&f.value==v&&f.canonical&&!f.userEmpty){d.fields[key]=f;d.intents.remove(key);return true;}
    d.fields[key].value=v;d.fields[key].known=true;d.intents[key]=AeroLoginIntent::Set;return true;
}
bool derived(AeroLoginSnapshot &d,const AeroLoginSnapshot &b,bool value,QString &error)
{
    const auto f=b.fields.value(AeroLoginKey::RdpBackground);
    if(f.known&&f.value==value){d.fields[AeroLoginKey::RdpBackground]=f;d.intents.remove(AeroLoginKey::RdpBackground);return true;}
    const auto pending=d.fields.value(AeroLoginKey::RdpBackground);
    if(pending.known&&pending.value==value&&d.intents.contains(AeroLoginKey::RdpBackground))return true;
    return rawStage(d,b,AeroLoginKey::RdpBackground,value,error);
}
bool prepared(const AeroLoginImage &i)
{
    return i.valid&&i.format==QLatin1String("png")&&!i.bytes.isEmpty()&&i.bytes.size()<=AeroLoginTypes::MaximumImageBytes
        &&i.digest==hash(i.bytes)&&i.width>0&&i.height>0&&qint64(i.width)*i.height<=AeroLoginTypes::MaximumPixels
        &&i.bytes.size()>=24&&i.bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))
        &&qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(i.bytes.constData()+16))==quint32(i.width)
        &&qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(i.bytes.constData()+20))==quint32(i.height);
}
}
bool AeroLoginField::operator==(const AeroLoginField &o) const
{return value==o.value&&defaultValue==o.defaultValue&&known==o.known&&defaultKnown==o.defaultKnown&&localPresent==o.localPresent&&basePresent==o.basePresent&&userEmpty==o.userEmpty&&canonical==o.canonical&&editorKnown==o.editorKnown&&editorValue==o.editorValue&&writable==o.writable&&raw==o.raw&&baseRaw==o.baseRaw&&reason==o.reason;}
bool AeroLoginImage::operator==(const AeroLoginImage &o) const
{return bytes==o.bytes&&digest==o.digest&&width==o.width&&height==o.height&&format==o.format&&reason==o.reason&&valid==o.valid;}
bool AeroLoginAsset::operator==(const AeroLoginAsset &o) const
{return present==o.present&&readable==o.readable&&size==o.size&&bytes==o.bytes&&digest==o.digest&&reason==o.reason;}
QList<AeroLoginKey> AeroLoginTypes::keys(){return {AeroLoginKey::StartupAnimation,AeroLoginKey::StartupSound,AeroLoginKey::ForceUserSelection,AeroLoginKey::RdpBackground};}
QString AeroLoginTypes::keyName(AeroLoginKey k)
{switch(k){case AeroLoginKey::StartupAnimation:return QStringLiteral("enableStartup");case AeroLoginKey::StartupSound:return QStringLiteral("playSound");case AeroLoginKey::ForceUserSelection:return QStringLiteral("forceUserSelect");case AeroLoginKey::RdpBackground:return QStringLiteral("rdpBackground");}return {};}
QString AeroLoginTypes::label(AeroLoginKey k)
{switch(k){case AeroLoginKey::StartupAnimation:return QObject::tr("Startup pearl animation");case AeroLoginKey::StartupSound:return QObject::tr("Startup sound");case AeroLoginKey::ForceUserSelection:return QObject::tr("Always show user selection");case AeroLoginKey::RdpBackground:return QObject::tr("Plain blue login background");}return {};}
bool AeroLoginTypes::builtinEditorDefault(AeroLoginKey k){return k==AeroLoginKey::StartupSound;}
bool AeroLoginTypes::sameConfiguration(const AeroLoginSnapshot &a,const AeroLoginSnapshot &b)
{return a.fields==b.fields&&a.background==b.background&&a.approvedDefault==b.approvedDefault&&a.revision==b.revision&&a.helperRevision==b.helperRevision&&a.sourceValid==b.sourceValid&&a.trustedInstallation==b.trustedInstallation&&a.writable==b.writable&&a.helperAvailable==b.helperAvailable&&a.recoveryPending==b.recoveryPending&&a.canRecover==b.canRecover&&a.reason==b.reason&&a.helperReason==b.helperReason&&a.recoverySummary==b.recoverySummary&&a.issues==b.issues;}
bool AeroLoginTypes::sameDraft(const AeroLoginSnapshot &a,const AeroLoginSnapshot &b)
{return sameConfiguration(a,b)&&a.intents==b.intents&&a.backgroundIntent==b.backgroundIntent&&a.replacement==b.replacement;}
bool AeroLoginTypes::dirty(const AeroLoginSnapshot &s){return !s.intents.isEmpty()||s.backgroundIntent!=AeroLoginBackgroundIntent::Keep;}
AeroLoginImage AeroLoginTypes::prepareImage(const QByteArray &bytes)
{
    AeroLoginImage out;
    if(bytes.isEmpty()||bytes.size()>MaximumImageBytes){out.reason=QObject::tr("Choose a PNG or JPEG no larger than 16 MiB.");return out;}
    QBuffer input;input.setData(bytes);input.open(QIODevice::ReadOnly);QImageReader reader(&input);reader.setDecideFormatFromContent(true);reader.setAutoTransform(true);
    const auto format=reader.format().toLower();const auto size=reader.size();
    if((format!="png"&&format!="jpeg"&&format!="jpg")||!size.isValid()||qint64(size.width())*size.height()>MaximumPixels){out.reason=QObject::tr("Choose a valid PNG or JPEG of at most 16 megapixels.");return out;}
    auto image=reader.read();if(image.isNull()||qint64(image.width())*image.height()>MaximumPixels){out.reason=QObject::tr("The image could not be decoded within the size limit.");return out;}
    if(image.colorSpace().isValid())image=image.convertedToColorSpace(QColorSpace::SRgb);
    image=image.convertToFormat(QImage::Format_RGBA8888);
    QImage clean(image.size(),QImage::Format_RGBA8888);if(clean.isNull()){out.reason=QObject::tr("The image could not be prepared.");return out;}
    for(int y=0;y<image.height();++y)memcpy(clean.scanLine(y),image.constScanLine(y),size_t(image.width())*4);
    // A fresh QImage inherits the application's default DPI (100 in QCore,
    // often 96 in the GUI). Pin PNG resolution metadata so independent decoder
    // processes produce the same canonical bytes for the same pixels.
    clean.setDotsPerMeterX(3780);clean.setDotsPerMeterY(3780);
    clean.setColorSpace(QColorSpace::SRgb);QBuffer encoded(&out.bytes);encoded.open(QIODevice::WriteOnly);
    if(!clean.save(&encoded,"PNG")||out.bytes.size()>MaximumImageBytes){out.bytes.clear();out.reason=QObject::tr("The prepared PNG exceeds 16 MiB. Choose a smaller picture.");return out;}
    out.width=clean.width();out.height=clean.height();out.format=QStringLiteral("png");out.digest=hash(out.bytes);out.valid=true;return out;
}
bool AeroLoginTypes::stage(AeroLoginSnapshot &d,const AeroLoginSnapshot &b,AeroLoginKey k,bool v,QString &error)
{
    error.clear();if(k==AeroLoginKey::RdpBackground&&v)return removeImage(d,b,error);
    auto next=d;if(!rawStage(next,b,k,v,error))return false;
    if(k==AeroLoginKey::RdpBackground&&next.backgroundIntent==AeroLoginBackgroundIntent::RemoveOverride){next.backgroundIntent=AeroLoginBackgroundIntent::Keep;next.replacement={};}
    d=next;return true;
}
bool AeroLoginTypes::restore(AeroLoginSnapshot &d,const AeroLoginSnapshot &b,AeroLoginKey k,QString &error)
{
    error.clear();const auto f=b.fields.value(k);
    if(!keys().contains(k)||!d.fields.contains(k)||!editable(b)||!f.writable||!f.defaultKnown){error=QObject::tr("The approved base default is unavailable or protected.");return false;}
    auto next=d;
    if(!f.localPresent){next.fields[k]=f;next.intents.remove(k);}
    else{next.fields[k].value=f.defaultValue;next.fields[k].known=true;next.intents[k]=AeroLoginIntent::RestoreApprovedDefault;}
    if(k==AeroLoginKey::RdpBackground){
        const bool value=next.fields[k].value;
        if(value){next.backgroundIntent=AeroLoginBackgroundIntent::RemoveOverride;next.replacement={};}
        else if(next.backgroundIntent==AeroLoginBackgroundIntent::RemoveOverride){next.backgroundIntent=AeroLoginBackgroundIntent::Keep;next.replacement={};}
    }
    d=next;return true;
}
bool AeroLoginTypes::selectImage(AeroLoginSnapshot &d,const AeroLoginSnapshot &b,const AeroLoginImage &i,QString &error)
{
    error.clear();if(!editable(b)||!prepared(i)){error=QObject::tr("Choose a validated prepared PNG for the approved Aero background.");return false;}
    auto next=d;if(!derived(next,b,false,error))return false;next.backgroundIntent=AeroLoginBackgroundIntent::ReplaceImage;next.replacement=i;d=next;return true;
}
bool AeroLoginTypes::removeImage(AeroLoginSnapshot &d,const AeroLoginSnapshot &b,QString &error)
{
    error.clear();if(!editable(b)){error=QObject::tr("The approved Aero background is not writable.");return false;}
    auto next=d;if(!derived(next,b,true,error))return false;next.backgroundIntent=AeroLoginBackgroundIntent::RemoveOverride;next.replacement={};d=next;return true;
}
bool AeroLoginTypes::restoreImage(AeroLoginSnapshot &d,const AeroLoginSnapshot &b,QString &error)
{
    error.clear();if(!editable(b)||!b.approvedDefault.present||!b.approvedDefault.readable||b.approvedDefault.bytes.isEmpty()||b.approvedDefault.bytes.size()>MaximumImageBytes||b.approvedDefault.digest!=hash(b.approvedDefault.bytes)){error=QObject::tr("The approved default background file is unavailable.");return false;}
    auto next=d;if(!derived(next,b,false,error))return false;next.backgroundIntent=AeroLoginBackgroundIntent::RestoreApprovedImage;next.replacement={};d=next;return true;
}
bool AeroLoginTypes::requestValid(const AeroLoginSnapshot &b,const AeroLoginSnapshot &d,QString &error)
{
    error.clear();if(!editable(b)||b.fields.size()!=keys().size()||!b.intents.isEmpty()||b.backgroundIntent!=AeroLoginBackgroundIntent::Keep){error=QObject::tr("The approved Aero source, installation or recovery state is unavailable.");return false;}
    auto next=b;for(auto it=d.intents.cbegin();it!=d.intents.cend();++it){
        if(!keys().contains(it.key())||!d.fields.contains(it.key())){error=QObject::tr("An unowned login preference was requested.");return false;}
        // Scalar intents are reconstructed without inferring extra image changes;
        // the explicit background intent below owns that coupled operation.
        if(it.value()==AeroLoginIntent::Set){if(!rawStage(next,b,it.key(),d.fields[it.key()].value,error))return false;}
        else if(it.value()==AeroLoginIntent::RestoreApprovedDefault){
            const auto f=b.fields.value(it.key());if(!f.writable||!f.defaultKnown||!f.localPresent){error=QObject::tr("The requested default operation is unavailable.");return false;}
            next.fields[it.key()].value=f.defaultValue;next.fields[it.key()].known=true;next.intents[it.key()]=it.value();
        }else{error=QObject::tr("An unknown Aero login operation was requested.");return false;}
    }
    switch(d.backgroundIntent){
    case AeroLoginBackgroundIntent::Keep:break;
    case AeroLoginBackgroundIntent::ReplaceImage:if(!selectImage(next,b,d.replacement,error))return false;break;
    case AeroLoginBackgroundIntent::RemoveOverride:if(!removeImage(next,b,error))return false;break;
    case AeroLoginBackgroundIntent::RestoreApprovedImage:if(!restoreImage(next,b,error))return false;break;
    default:error=QObject::tr("An unknown background operation was requested.");return false;
    }
    if(!sameDraft(next,d)){error=QObject::tr("The request changed an unowned source, capability or background companion.");return false;}
    if(d.intents.contains(AeroLoginKey::RdpBackground)&&d.fields[AeroLoginKey::RdpBackground].value&&d.backgroundIntent!=AeroLoginBackgroundIntent::RemoveOverride){error=QObject::tr("The blue-background choice must explicitly remove the image override.");return false;}
    return true;
}
bool AeroLoginTypes::matchesRequest(const AeroLoginSnapshot &b,const AeroLoginSnapshot &d,const AeroLoginSnapshot &a,QString &error)
{
    if(!requestValid(b,d,error))return false;
    if(!a.sourceValid||!a.trustedInstallation||!a.writable||a.recoveryPending||a.helperAvailable!=b.helperAvailable||a.helperRevision!=b.helperRevision||dirty(a)||a.fields.size()!=4||!(a.approvedDefault==b.approvedDefault)){error=QObject::tr("Aero source, default image, installation or recovery state changed during saving.");return false;}
    for(auto k:keys()){
        const auto before=b.fields.value(k),after=a.fields.value(k);
        if(!d.intents.contains(k)){auto storedBefore=before,storedAfter=after;
            // Creating .user changes the buggy native editor's copyFrom-derived
            // projection. It does not change these untouched SDDM inputs.
            storedBefore.editorKnown=storedAfter.editorKnown=false;storedBefore.editorValue=storedAfter.editorValue=false;storedBefore.reason=storedAfter.reason={};
            if(!(storedBefore==storedAfter)){error=QObject::tr("An untouched login preference changed.");return false;}continue;}
        if(!after.known||after.value!=d.fields.value(k).value||after.baseRaw!=before.baseRaw||after.basePresent!=before.basePresent||after.defaultKnown!=before.defaultKnown||after.defaultValue!=before.defaultValue||after.writable!=before.writable){error=QObject::tr("A saved login preference or its base default differs from the reviewed change.");return false;}
        const bool explicitValue=d.intents[k]==AeroLoginIntent::Set;
        if(explicitValue?(!after.localPresent||!after.canonical||after.userEmpty):after.localPresent){error=QObject::tr("The requested login override or default removal could not be verified.");return false;}
    }
    if(d.backgroundIntent==AeroLoginBackgroundIntent::Keep){if(!(a.background==b.background)){error=QObject::tr("The untouched background changed.");return false;}}
    else if(d.backgroundIntent==AeroLoginBackgroundIntent::RemoveOverride){if(a.background.present){error=QObject::tr("The background override is still present.");return false;}}
    else{const auto digest=d.backgroundIntent==AeroLoginBackgroundIntent::ReplaceImage?d.replacement.digest:b.approvedDefault.digest;if(!a.background.present||!a.background.readable||a.background.digest!=digest){error=QObject::tr("The saved background differs from the reviewed image.");return false;}}
    return true;
}
QString AeroLoginTypes::loginProjection(const AeroLoginSnapshot &d)
{
    const bool image=d.backgroundIntent==AeroLoginBackgroundIntent::ReplaceImage||d.backgroundIntent==AeroLoginBackgroundIntent::RestoreApprovedImage||(d.backgroundIntent==AeroLoginBackgroundIntent::Keep&&d.background.present);
    return image?QObject::tr("Login screen: the shared picture is expected on a future greeter. Animation, sound and user selection apply only to login; startup animation and sound also depend on the native startup marker."):QObject::tr("Login screen: no picture override; the installed theme is expected to show its plain blue background on a future greeter.");
}
QString AeroLoginTypes::lockProjection(const AeroLoginSnapshot &d)
{
    if(d.backgroundIntent==AeroLoginBackgroundIntent::RemoveOverride||d.backgroundIntent==AeroLoginBackgroundIntent::Keep&&!d.background.present)
        return d.approvedDefault.present?QObject::tr("Lock screen: the approved default picture is the expected fallback, even when login uses plain blue. Existing lock screens may retain their earlier background."):QObject::tr("Lock screen: no approved picture fallback is available; rendered behavior is unverified.");
    return QObject::tr("Lock screen: the shared picture is expected for a future lock screen. Login-only options do not change lock-screen animation or sounds; live refresh is unverified.");
}
QStringList AeroLoginTypes::reviewChanges(const AeroLoginSnapshot &b,const AeroLoginSnapshot &d)
{
    QStringList out;for(auto k:keys())if(d.intents.contains(k))out<<QObject::tr("%1: %2 → %3 (%4)").arg(label(k),b.fields[k].known?(b.fields[k].value?QObject::tr("enabled"):QObject::tr("disabled")):QObject::tr("unknown"),d.fields[k].value?QObject::tr("enabled"):QObject::tr("disabled"),d.intents[k]==AeroLoginIntent::Set?QObject::tr("explicit override"):QObject::tr("remove owned override; use base default"));
    if(d.backgroundIntent==AeroLoginBackgroundIntent::ReplaceImage)out<<QObject::tr("Replace the shared background with a prepared %1 × %2 PNG (SHA-256 %3).").arg(d.replacement.width).arg(d.replacement.height).arg(QString::fromLatin1(d.replacement.digest.toHex()));
    if(d.backgroundIntent==AeroLoginBackgroundIntent::RemoveOverride)out<<QObject::tr("Remove the background override. Login is expected to use blue; locking is expected to fall back to the approved default image.");
    if(d.backgroundIntent==AeroLoginBackgroundIntent::RestoreApprovedImage)out<<QObject::tr("Copy the approved default picture into the shared background override (SHA-256 %1).").arg(QString::fromLatin1(b.approvedDefault.digest.toHex()));
    return out;
}
