// Isolated subprocess fixture. Only the test guard is linked to this program.
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString path = qEnvironmentVariable("CONTROL_DISPLAY_TEST_STATE");
    if (path.isEmpty()) return 2;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 3;
    QJsonObject state = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    const QStringList arguments = app.arguments().mid(1);
    QJsonObject config = state.value("config").toObject();
    if (arguments == QStringList{QStringLiteral("--json")}) {
        if (state.value("failReads").toBool()) return 4;
        const QByteArray bytes = QJsonDocument(config).toJson(QJsonDocument::Compact);
        fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        return 0;
    }
    const bool first = state.value("writes").toInt() == 0;
    const int limit = first ? state.value("partialArguments").toInt(arguments.size()) : arguments.size();
    auto outputs = config.value("outputs").toArray();
    int applied=0;
    for (const auto &argument : arguments) {
        const auto parts=argument.split(QLatin1Char('.'));
        if (parts.size()<3 || parts[0]!=QLatin1String("output")) return 5;
        int index=-1;
        for(int i=0;i<outputs.size();++i) {
            const auto output=outputs[i].toObject();
            if(output.value("name").toString()==parts[1] || QString::number(output.value("id").toInt())==parts[1]) index=i;
        }
        if(index<0)return 5;
        if (applied++ >= limit) continue;
        auto output=outputs[index].toObject(); const auto key=parts[2];
        const auto value=parts.mid(3).join(QLatin1Char('.'));
        if(key==QLatin1String("mode")) {
            if (!(state.value("mismatch").toBool() && value==QLatin1String("2"))) output.insert("currentModeId",value);
        } else if(key==QLatin1String("enable")) output.insert("enabled",true);
        else if(key==QLatin1String("disable")) output.insert("enabled",false);
        else if(key==QLatin1String("scale")) output.insert("scale",std::round(value.toDouble()*120)/120);
        else if(key==QLatin1String("position")) {const auto xy=value.split(QLatin1Char(','));if(xy.size()!=2)return 5;output.insert("pos",QJsonObject{{"x",xy[0].toInt()},{"y",xy[1].toInt()}});}
        else if(key==QLatin1String("priority")) output.insert("priority",value.toInt());
        else if(key==QLatin1String("rotation")) {
            const QStringList rotations{"normal","left","inverted","right","flipped","flipped90","flipped180","flipped270"};
            const int rotation=rotations.indexOf(value);if(rotation<0)return 5;output.insert("rotation",1<<rotation);
        } else return 5;
        outputs[index]=output;
    }
    // Native SetConfigOperation shifts enabled positions to the origin and the
    // Wayland backend compacts enabled priorities before submitting one batch.
    int x=1000001,y=1000001; QList<int> active;
    for(int i=0;i<outputs.size();++i) {const auto output=outputs[i].toObject();if(!output.value("enabled").toBool())continue;active<<i;
        if(!output.value("replicationSource").toInt()){const auto pos=output.value("pos").toObject();x=qMin(x,pos.value("x").toInt());y=qMin(y,pos.value("y").toInt());}}
    std::sort(active.begin(),active.end(),[&](int a,int b){const auto left=outputs[a].toObject(),right=outputs[b].toObject();const int l=left.value("priority").toInt(),r=right.value("priority").toInt();return l==r?left.value("name").toString()<right.value("name").toString():l<r;});
    for(int rank=0;rank<active.size();++rank) {const int i=active[rank];auto output=outputs[i].toObject();const auto pos=output.value("pos").toObject();
        if(x<=1000000&&y<=1000000)output.insert("pos",QJsonObject{{"x",pos.value("x").toInt()-x},{"y",pos.value("y").toInt()-y}});
        output.insert("priority",rank+1);outputs[i]=output;}
    config.insert("outputs", outputs); state.insert("config", config);
    state.insert("writes", state.value("writes").toInt() + 1);
    auto batches=state.value("batches").toArray();batches.append(QJsonArray::fromStringList(arguments));state.insert("batches",batches);
    state.insert("doctorPlatform",qEnvironmentVariable("QT_QPA_PLATFORM"));
    state.insert("backendOverride",qEnvironmentVariable("KSCREEN_BACKEND"));
    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly)) return 6;
    save.write(QJsonDocument(state).toJson());
    if(!save.commit())return 7;
    if(first && state.value("hangFirstWrite").toBool())QThread::msleep(6500);
    return first && state.value("failFirstWrite").toBool() ? 8 : 0;
}
