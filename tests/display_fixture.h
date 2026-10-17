#pragma once
#include "display/DisplayProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>

inline DisplaySnapshot displayFixture(const QString &current = QStringLiteral("1"))
{
    QJsonArray modes;
    modes.append(QJsonObject{{"id", "1"}, {"name", "1920x1080@60"}, {"refreshRate", 60},
                             {"size", QJsonObject{{"width", 1920}, {"height", 1080}}}});
    modes.append(QJsonObject{{"id", "2"}, {"name", "1280x720@60"}, {"refreshRate", 60},
                             {"size", QJsonObject{{"width", 1280}, {"height", 720}}}});
    QJsonObject output{{"id", 1}, {"name", "eDP-1"}, {"connected", true}, {"enabled", true},
                       {"currentModeId", current}, {"modes", modes}, {"scale", 1}, {"rotation", 1},
                       {"pos", QJsonObject{{"x", 0}, {"y", 0}}}, {"priority", 1},
                       {"size", QJsonObject{{"width", 1920}, {"height", 1080}}},
                       {"sizeMM", QJsonObject{{"width", 310}, {"height", 170}}},
                       {"followPreferredMode", false}, {"replicationSource", 0}};
    QJsonObject config{{"outputs", QJsonArray{output}}, {"features", 255},
                       {"screen", QJsonObject{{"maxActiveOutputsCount", 1},
                           {"currentSize", QJsonObject{{"width", 1920}, {"height", 1080}}}}}};
    DisplaySnapshot snapshot;
    QString error;
    DisplayProtocol::parse(QJsonDocument(config).toJson(), snapshot, error);
    return snapshot;
}

inline DisplaySnapshot displayMultiFixture()
{
    auto config = displayFixture().json;
    auto outputs = config.value("outputs").toArray();
    auto second = outputs[0].toObject();
    second.insert("id", 2); second.insert("name", "HDMI-A-1");
    second.insert("pos", QJsonObject{{"x", 1920}, {"y", 0}});
    second.insert("priority", 2); outputs.append(second);
    config.insert("outputs", outputs);
    auto screen = config.value("screen").toObject();
    screen.insert("maxActiveOutputsCount", 2);
    screen.insert("currentSize", QJsonObject{{"width", 3840}, {"height", 1080}});
    config.insert("screen", screen);
    DisplaySnapshot snapshot; QString error;
    DisplayProtocol::parse(QJsonDocument(config).toJson(), snapshot, error);
    return snapshot;
}

inline DisplaySnapshot displayTripleFixture()
{
    auto config = displayMultiFixture().json;
    auto outputs = config.value("outputs").toArray();
    auto third = outputs[0].toObject();
    third.insert("id", 3); third.insert("name", "DP-2");
    third.insert("pos", QJsonObject{{"x", 3840}, {"y", 0}});
    third.insert("priority", 3); outputs.append(third);
    config.insert("outputs", outputs);
    auto screen = config.value("screen").toObject();
    screen.insert("maxActiveOutputsCount", 3);
    screen.insert("currentSize", QJsonObject{{"width", 5760}, {"height", 1080}});
    config.insert("screen", screen);
    DisplaySnapshot snapshot; QString error;
    DisplayProtocol::parse(QJsonDocument(config).toJson(), snapshot, error);
    return snapshot;
}

inline DisplaySnapshot displayModified(DisplaySnapshot snapshot, const QString &key,
                                       const QJsonValue &value)
{
    auto outputs = snapshot.json.value("outputs").toArray();
    auto output = outputs.at(0).toObject();
    output.insert(key, value); outputs[0] = output;
    snapshot.json.insert("outputs", outputs);
    DisplaySnapshot changed; QString error;
    DisplayProtocol::parse(QJsonDocument(snapshot.json).toJson(), changed, error);
    return changed;
}
