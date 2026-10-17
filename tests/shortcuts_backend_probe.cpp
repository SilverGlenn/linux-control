#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include "platform/shortcuts/ShortcutsBackend.h"

int main(int argc,char **argv)
{
    QApplication app(argc,argv);
    // This probe contains no writeChanges call, action registration, purge, invocation,
    // context activation, or KCM launch. It only reads the provider's bounded snapshot.
    KdeShortcutsBackend backend;
    QObject::connect(&backend,&ShortcutsBackend::readFinished,&app,[&](quint64,bool success,const ShortcutsSnapshot &state,const QString &error){
        QJsonArray actions;
        for(const auto &action:state.actions){
            QJsonArray current,defaults;
            for(const auto &key:action.current)current<<key.toString(QKeySequence::PortableText);
            for(const auto &key:action.defaults)defaults<<key.toString(QKeySequence::PortableText);
            actions<<QJsonObject{{QStringLiteral("component"),action.id.component},{QStringLiteral("context"),action.id.context},{QStringLiteral("action"),action.id.action},
                {QStringLiteral("componentLabel"),action.componentFriendly},{QStringLiteral("contextLabel"),action.contextFriendly},{QStringLiteral("label"),action.friendlyName},
                {QStringLiteral("current"),current},{QStringLiteral("defaults"),defaults},{QStringLiteral("editable"),action.available},
                {QStringLiteral("registered"),action.registered},{QStringLiteral("layoutAction"),action.layoutAction}};
        }
        QTextStream(stdout)<<QJsonDocument(QJsonObject{{QStringLiteral("success"),success},{QStringLiteral("owner"),state.owner},{QStringLiteral("available"),state.available},
            {QStringLiteral("writable"),state.writable},{QStringLiteral("reason"),state.reason},{QStringLiteral("error"),error},{QStringLiteral("revision"),QString::fromLatin1(state.revision.toHex())},
            {QStringLiteral("configRevision"),QString::fromLatin1(state.configRevision.toHex())},{QStringLiteral("actions"),actions},
            {QStringLiteral("layoutAdapter"),QJsonObject{{QStringLiteral("available"),state.layouts.available},
                {QStringLiteral("owner"),state.layouts.owner},{QStringLiteral("version"),state.layouts.version},
                {QStringLiteral("reason"),state.layouts.reason},{QStringLiteral("actionNames"),QJsonArray::fromStringList(state.layouts.actionNames)},
                {QStringLiteral("revision"),QString::fromLatin1(state.layouts.revision.toHex())},
                {QStringLiteral("configRevision"),QString::fromLatin1(state.layouts.configRevision.toHex())}}}}).toJson(QJsonDocument::Indented);
        app.exit(success?0:1);
    });
    QTimer::singleShot(0,&backend,[&]{backend.readCurrent(1);});
    QTimer::singleShot(35000,&app,[&]{app.exit(2);});
    return app.exec();
}
