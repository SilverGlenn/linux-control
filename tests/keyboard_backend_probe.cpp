#include "platform/keyboard/KeyboardBackend.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    // Read-only by construction: no writePreferences, native notifications,
    // layout switches, KCM process or compositor reconfigure calls.
    KdeKeyboardBackend backend;KeyboardSnapshot first;bool haveFirst=false;
    QObject::connect(&backend,&KeyboardBackend::readFinished,&app,[&](quint64,bool success,const KeyboardSnapshot &state,const QString &error){
        if(success&&!haveFirst){first=state;haveFirst=true;backend.readCurrent(2);return;}
        const bool stable=haveFirst&&success&&first.owner==state.owner&&first.revision==state.revision&&first.registry.revision==state.registry.revision;
        QJsonArray runtime,immutable;
        for(const auto &layout:state.runtimeLayouts)runtime<<QJsonObject{{"shortName",layout.shortName},{"displayName",layout.displayName},{"longName",layout.longName}};
        for(const auto &key:state.immutableKeys)immutable<<key;
        QJsonObject result{{"readOnly",true},{"success",success},{"repeatedReadStable",stable},{"owner",state.owner},{"version",state.version},
            {"available",state.available},{"writable",state.writable},{"reloadAvailable",state.reloadAvailable},{"reason",state.reason},{"error",error},
            {"runtimeStatus",state.runtimeStatus},{"runtimeLayoutsVerified",state.runtimeLayoutsVerified},{"runtimeLayouts",runtime},
            {"values",QJsonObject::fromVariantMap(state.values)},{"immutableKeys",immutable},{"registryValid",state.registry.valid},
            {"models",state.registry.models.size()},{"layouts",state.registry.layouts.size()},{"optionGroups",state.registry.optionGroups.size()}};
        QTextStream(stdout)<<QJsonDocument(result).toJson(QJsonDocument::Indented);app.exit(success&&stable?0:1);
    });
    QTimer::singleShot(25000,&app,[&]{QTextStream(stderr)<<"Read-only keyboard probe timed out.\n";app.exit(2);});
    QTimer::singleShot(0,&backend,[&]{backend.readCurrent(1);});return app.exec();
}
