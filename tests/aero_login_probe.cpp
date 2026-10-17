#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cstdio>
#include "login/AeroLoginFiles.h"
#include "platform/kde/AeroLoginBackend.h"
// Passive fixed-path evidence only: no image decoder, QML, native KCM, service
// calls, authorization, helper execution, preference setters or greeter launch.
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);QString error;const auto s=AeroLoginFiles::read({},&error);const auto helper=inspectAeroLoginHelper();QJsonArray fields;
    for(auto key:AeroLoginTypes::keys()){const auto f=s.fields.value(key);fields.append(QJsonObject{{"key",AeroLoginTypes::keyName(key)},{"known",f.known},{"value",f.value},{"localPresent",f.localPresent},{"canonical",f.canonical},{"writable",f.writable},{"editorKnown",f.editorKnown},{"reason",f.reason}});}
    const QJsonObject o{{"sourceValid",s.sourceValid},{"trustedInstallation",s.trustedInstallation},{"writable",s.writable},{"sourceRevision",QString::fromLatin1(s.revision.toHex())},{"fields",fields},{"backgroundPresent",s.background.present},{"backgroundSha256",QString::fromLatin1(s.background.digest.toHex())},{"approvedDefaultSha256",QString::fromLatin1(s.approvedDefault.digest.toHex())},{"recoveryPending",s.recoveryPending},{"canRecover",s.canRecover},{"helperAvailable",helper.available},{"helperReason",helper.reason},{"reason",s.reason},{"error",error}};
    const auto bytes=QJsonDocument(o).toJson();return std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout)==size_t(bytes.size())?0:1;
}
