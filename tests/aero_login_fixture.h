#pragma once
#include "settings/AeroLoginSettings.h"
#include <QImage>
#include <QBuffer>
#include <QCryptographicHash>
inline AeroLoginImage aeroLoginImageFixture(const QColor &color=QColor(80,130,180))
{QImage picture(16,12,QImage::Format_RGB32);picture.fill(color);QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);picture.save(&buffer,"PNG");return AeroLoginTypes::prepareImage(bytes);}
inline AeroLoginSnapshot aeroLoginFixture()
{
    AeroLoginSnapshot s;s.sourceValid=s.trustedInstallation=s.writable=s.helperAvailable=true;s.revision=QCryptographicHash::hash("fixture-source-revision",QCryptographicHash::Sha256);s.helperRevision=QCryptographicHash::hash("fixture-helper-revision",QCryptographicHash::Sha256);
    for(auto key:AeroLoginTypes::keys()){auto &f=s.fields[key];f.value=f.defaultValue=AeroLoginTypes::builtinEditorDefault(key);f.known=f.defaultKnown=f.basePresent=f.writable=f.editorKnown=true;f.editorValue=f.value;f.baseRaw=f.value?QStringLiteral("true"):QStringLiteral("false");}
    const auto image=aeroLoginImageFixture();s.background={true,true,image.bytes.size(),image.bytes,image.digest,{}};const auto defaultImage=aeroLoginImageFixture(QColor(110,160,210));s.approvedDefault={true,true,defaultImage.bytes.size(),defaultImage.bytes,defaultImage.digest,{}};return s;
}
inline AeroLoginSnapshot savedAeroLoginFixture(const AeroLoginSnapshot &requested)
{
    auto actual=requested;
    for(auto it=requested.intents.cbegin();it!=requested.intents.cend();++it){auto &f=actual.fields[it.key()];f.localPresent=it.value()==AeroLoginIntent::Set;f.raw=f.localPresent?(f.value?QStringLiteral("true"):QStringLiteral("false")):QString();f.userEmpty=false;f.canonical=true;f.editorValue=f.value;f.editorKnown=true;}
    if(requested.backgroundIntent==AeroLoginBackgroundIntent::ReplaceImage){const auto &i=requested.replacement;actual.background={true,true,i.bytes.size(),i.bytes,i.digest,{}};}
    else if(requested.backgroundIntent==AeroLoginBackgroundIntent::RestoreApprovedImage)actual.background=actual.approvedDefault;
    else if(requested.backgroundIntent==AeroLoginBackgroundIntent::RemoveOverride)actual.background={};
    actual.intents.clear();actual.backgroundIntent=AeroLoginBackgroundIntent::Keep;actual.replacement={};actual.revision=QCryptographicHash::hash(actual.revision+"-saved",QCryptographicHash::Sha256);return actual;
}
