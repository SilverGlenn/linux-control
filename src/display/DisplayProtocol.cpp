#include "DisplayProtocol.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QRectF>
#include <QSizeF>
#include <algorithm>
#include <cmath>

QString DisplayMode::label() const { return QStringLiteral("%1 × %2, %3 Hz").arg(width).arg(height).arg(refresh, 0, 'f', 2); }
const DisplayOutput *DisplaySnapshot::output(int id) const { for (const auto &item : outputs) if (item.id == id) return &item; return nullptr; }
DisplayOutput *DisplaySnapshot::output(int id) { for (auto &item : outputs) if (item.id == id) return &item; return nullptr; }
namespace {
constexpr int writable = 2, scaling = 4, synchronous = 64;
bool integer(const QJsonValue &value, int minimum, int maximum) {
    const double n = value.toDouble(-1e20); return value.isDouble() && std::isfinite(n) && n == std::floor(n) && n >= minimum && n <= maximum;
}
bool token(const QString &value) { static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_-]{1,128}$")); return safe.match(value).hasMatch(); }
bool rotation(int value) { return value >= 1 && value <= 128 && !(value & (value - 1)); }
QStringList mutableKeys() { return {QStringLiteral("enabled"),QStringLiteral("currentModeId"),QStringLiteral("pos"),QStringLiteral("scale"),QStringLiteral("rotation"),QStringLiteral("priority")}; }
QJsonObject projection(const DisplaySnapshot &snapshot, bool layout) {
    QJsonArray outputs;
    for (const auto &value : snapshot.json.value("outputs").toArray()) {
        const auto source = value.toObject(); QJsonObject output;
        const QStringList identity{QStringLiteral("id"),QStringLiteral("name"),QStringLiteral("type"),QStringLiteral("sizeMM"),QStringLiteral("connected"),
            QStringLiteral("preferredModes"),QStringLiteral("modes"),QStringLiteral("replicationSource"),QStringLiteral("clones"),QStringLiteral("followPreferredMode")};
        for (const auto &key : identity) if (source.contains(key)) output.insert(key, source.value(key));
        if (layout) for (const auto &key : mutableKeys()) output.insert(key, source.value(key));
        if (layout) {
            if (!source.value("enabled").toBool()) output.insert("priority", 0); // Disabled priority is not part of the active output order.
        }
        outputs << output;
    }
    auto screen = snapshot.json.value("screen").toObject(); screen.remove("currentSize");
    return {{"outputs", outputs},{"screen", screen},{"features", snapshot.json.value("features")},{"tabletModeAvailable", snapshot.json.value("tabletModeAvailable")},{"tabletModeEngaged", snapshot.json.value("tabletModeEngaged")},{"controlIdentity", snapshot.json.value("controlIdentity")}};
}
bool writableScale(double scale)
{
    return std::isfinite(scale) && scale >= DisplayProtocol::MinimumScale
        && scale <= DisplayProtocol::MaximumWrittenScale
        && std::round(scale*120)/120 == scale;
}

// SetConfigOperation subtracts one coordinatewise origin from every connected
// enabled output, after a possibly partial enable/position change. A position
// can therefore differ from both saved endpoints while still being ours. Prove
// the whole layout came from endpoint QPoint choices and one common offset;
// never grant each monitor its own offset or mix endpoint x/y independently.
bool ownedPositions(const DisplaySnapshot &baseline, const DisplaySnapshot &target,
                    const DisplaySnapshot &actual)
{
    const DisplayOutput *anchor = nullptr;
    for (const auto &output : actual.outputs) {
        if (output.connected && output.enabled && !output.replicationSource) {
            anchor = &output;
            break;
        }
    }
    // Native configuration rejects all-off. Without a positionable anchor we
    // cannot prove an origin shift; preserve the unknown state without writing.
    if (!anchor) return false;
    const auto *beforeAnchor = baseline.output(anchor->id);
    const auto *targetAnchor = target.output(anchor->id);
    if (!beforeAnchor || !targetAnchor) return false;
    const QList<QPoint> offsets{beforeAnchor->position-anchor->position,
                               targetAnchor->position-anchor->position};
    for (const auto &offset : offsets) {
        bool valid = true;
        int minX = DisplayProtocol::MaximumPosition+1, minY = DisplayProtocol::MaximumPosition+1;
        for (const auto &output : actual.outputs) {
            const auto *before = baseline.output(output.id);
            const auto *desired = target.output(output.id);
            if (!before || !desired) { valid = false; break; }
            const QPoint source = output.connected && output.enabled
                ? output.position+offset : output.position;
            if (source != before->position && source != desired->position) {
                valid = false;
                break;
            }
            if (output.connected && output.enabled && !output.replicationSource) {
                minX = qMin(minX, source.x());
                minY = qMin(minY, source.y());
            }
        }
        if (valid && QPoint(minX,minY) == offset) return true;
    }
    return false;
}
QString rotationName(int value) {
    switch(value) { case 1:return QStringLiteral("normal");case 2:return QStringLiteral("left");case 4:return QStringLiteral("inverted");case 8:return QStringLiteral("right");
    case 16:return QStringLiteral("flipped");case 32:return QStringLiteral("flipped90");case 64:return QStringLiteral("flipped180");case 128:return QStringLiteral("flipped270");default:return {}; }
}
}
bool DisplayProtocol::parse(const QByteArray &bytes, DisplaySnapshot &snapshot, QString &error) {
    snapshot = {}; error = QStringLiteral("KScreen returned an unsupported or incomplete display configuration.");
    if (bytes.size() > 1024 * 1024) return false;
    QJsonParseError parseError; const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return false;
    auto config = document.object();
    if (!config.value("outputs").isArray() || !config.value("screen").isObject() || !integer(config.value("features"),0,2147483647)) return false;
    const auto values = config.value("outputs").toArray(); if (values.isEmpty() || values.size() > 64) return false;
    QSet<int> ids; QSet<QString> names; QList<QJsonObject> normalizedOutputs; DisplaySnapshot parsed;
    for (const auto &value : values) {
        if (!value.isObject()) return false; auto object = value.toObject(); const auto pos = object.value("pos").toObject();
        const double factor = object.value("scale").toDouble();
        if (!integer(object.value("id"),1,2147483647) || !object.value("name").isString() || object.value("name").toString().isEmpty() || object.value("name").toString().size()>255
            || !object.value("connected").isBool() || !object.value("enabled").isBool() || !object.value("currentModeId").isString() || !object.value("modes").isArray()
            || !object.value("pos").isObject() || !integer(pos.value("x"),-MaximumPosition,MaximumPosition) || !integer(pos.value("y"),-MaximumPosition,MaximumPosition)
            || !object.value("scale").isDouble() || !std::isfinite(factor) || factor<MinimumScale || factor>MaximumReadScale
            || !integer(object.value("rotation"),1,128) || !rotation(object.value("rotation").toInt()) || !object.value("sizeMM").isObject()
            || !integer(object.value("replicationSource"),0,2147483647) || !object.value("followPreferredMode").isBool()
            || !integer(object.value("priority"),0,2147483647)) return false;
        DisplayOutput output; output.id=object.value("id").toInt(); output.name=object.value("name").toString();
        if (ids.contains(output.id) || names.contains(output.name)) return false; ids.insert(output.id); names.insert(output.name);
        output.connected=object.value("connected").toBool(); output.enabled=object.value("enabled").toBool(); output.currentMode=object.value("currentModeId").toString();
        output.position={pos.value("x").toInt(),pos.value("y").toInt()}; output.scale=factor; output.rotation=object.value("rotation").toInt(); output.priority=object.value("priority").toInt();
        output.replicationSource=object.value("replicationSource").toInt(); output.followPreferredMode=object.value("followPreferredMode").toBool();
        QSet<QString> modeIds; QList<QJsonObject> modes; const auto modeValues=object.value("modes").toArray(); if(modeValues.size()>4096)return false;
        for(const auto &modeValue:modeValues) {
            if(!modeValue.isObject())return false; const auto mode=modeValue.toObject(),size=mode.value("size").toObject(); const auto id=mode.value("id").toString(); const double refresh=mode.value("refreshRate").toDouble();
            if(!token(id)||modeIds.contains(id)||!integer(size.value("width"),1,65536)||!integer(size.value("height"),1,65536)||!std::isfinite(refresh)||refresh<=0||refresh>10000)return false;
            modeIds.insert(id); output.modes << DisplayMode{id,size.value("width").toInt(),size.value("height").toInt(),refresh}; modes << mode;
        }
        if(output.enabled&&(!output.connected||!modeIds.contains(output.currentMode)))return false;
        std::sort(modes.begin(),modes.end(),[](const auto &a,const auto &b){return a.value("id").toString()<b.value("id").toString();}); QJsonArray sortedModes; for(const auto &mode:modes)sortedModes<<mode; object.insert("modes",sortedModes);
        normalizedOutputs << object; parsed.outputs << output;
    }
    std::sort(normalizedOutputs.begin(),normalizedOutputs.end(),[](const auto &a,const auto &b){return a.value("id").toInt()<b.value("id").toInt();});
    std::sort(parsed.outputs.begin(),parsed.outputs.end(),[](const auto &a,const auto &b){return a.id<b.id;});
    QJsonArray sortedOutputs;for(const auto &output:normalizedOutputs)sortedOutputs<<output;config.insert("outputs",sortedOutputs);
    const auto screen=config.value("screen").toObject();
    if(screen.contains("maxActiveOutputsCount")&&!integer(screen.value("maxActiveOutputsCount"),0,64))return false;
    if(config.contains("controlIdentity")) { const auto identity=config.value("controlIdentity").toObject(); if(identity.value("owner").toString().isEmpty()||identity.value("kwinVersion").toString()!=QLatin1String("6.6.6")||identity.value("libkscreenVersion").toString()!=QLatin1String("6.6.4"))return false; }
    parsed.json=config;snapshot=parsed;error.clear();return true;
}
DisplaySnapshot DisplayProtocol::normalized(const DisplaySnapshot &snapshot) {
    if(!snapshot.isValid())return {};
    auto candidate=snapshot;
    int x=MaximumPosition+1,y=MaximumPosition+1;
    QList<DisplayOutput *> enabled;
    const auto originalValues=snapshot.json.value("outputs").toArray();
    for(auto &output:candidate.outputs) {
        if(output.position.x() < -MaximumPosition || output.position.x() > MaximumPosition
            || output.position.y() < -MaximumPosition || output.position.y() > MaximumPosition) return {};
        const auto original=std::find_if(originalValues.begin(),originalValues.end(),[&](const auto &value){return value.toObject().value("id").toInt()==output.id;});
        if(original==originalValues.end())return {};
        const double savedScale=(*original).toObject().value("scale").toDouble();
        if(output.scale!=savedScale) {
            // Validate before multiplying to avoid overflow/NaN conversion.
            if(!std::isfinite(output.scale)||output.scale<=0||output.scale>MaximumReadScale)return {};
            output.scale=std::round(output.scale*120)/120;
            if(!writableScale(output.scale))return {};
        }
        if(output.connected&&output.enabled&&!output.replicationSource){x=qMin(x,output.position.x());y=qMin(y,output.position.y());}
        if(output.enabled)enabled<<&output;
    }
    if(x<=MaximumPosition&&y<=MaximumPosition)for(auto &output:candidate.outputs)if(output.connected&&output.enabled)output.position -= QPoint(x,y);
    std::stable_sort(enabled.begin(),enabled.end(),[](const auto *a,const auto *b){return a->priority==b->priority?a->name<b->name:a->priority<b->priority;});
    for(int i=0;i<enabled.size();++i)enabled[i]->priority=i+1;
    auto values=candidate.json.value("outputs").toArray();
    for(qsizetype index=0; index<values.size(); ++index) {auto object=values[index].toObject();const auto *output=candidate.output(object.value("id").toInt());if(!output)return {};
        object.insert("enabled",output->enabled);object.insert("currentModeId",output->currentMode);object.insert("pos",QJsonObject{{"x",output->position.x()},{"y",output->position.y()}});
        object.insert("scale",output->scale);object.insert("rotation",output->rotation);object.insert("priority",output->priority);object.insert("followPreferredMode",output->followPreferredMode);object.insert("replicationSource",output->replicationSource);values[index]=object;
    }
    candidate.json.insert("outputs",values); DisplaySnapshot result;QString error;if(!parse(QJsonDocument(candidate.json).toJson(),result,error))return {};return result;
}
bool DisplayProtocol::same(const DisplaySnapshot &a,const DisplaySnapshot &b) {return a.isValid()&&b.isValid()&&projection(a,true)==projection(b,true);}
bool DisplayProtocol::canPosition(const DisplaySnapshot &snapshot){return snapshot.isValid()&&(snapshot.json.value("features").toInt()&(writable|synchronous))==(writable|synchronous);}
bool DisplayProtocol::canScale(const DisplaySnapshot &snapshot){return canPosition(snapshot)&&(snapshot.json.value("features").toInt()&scaling);}
bool DisplayProtocol::canRotate(const DisplaySnapshot &snapshot){return canPosition(snapshot);}
bool DisplayProtocol::canEdit(const DisplaySnapshot &snapshot,int id,QString &error) {
    const auto *output=snapshot.output(id);
    if(!canPosition(snapshot)||!output||!output->connected){error=QStringLiteral("This output is unavailable or the native backend does not support guarded display changes.");return false;}
    if(!token(output->name)){error=QStringLiteral("The native output name cannot be addressed safely by the installed display tool.");return false;}
    bool mirrored=output->replicationSource!=0;for(const auto &other:snapshot.outputs)if(other.replicationSource==id)mirrored=true;
    if(mirrored){error=QStringLiteral("Mirrored outputs are read-only in this editor.");return false;}
    error.clear();return true;
}
bool DisplayProtocol::canChange(const DisplaySnapshot &baseline,const DisplaySnapshot &requested,QString &error) {
    const auto target=normalized(requested);if(!baseline.isValid()||!target.isValid()||projection(baseline,false)!=projection(target,false)){error=QStringLiteral("Output identity, topology, native policy, or advertised modes changed. Reload the displays.");return false;}
    int enabled=0;QRectF bounds;
    for(const auto &output:target.outputs) {
        const auto *before=baseline.output(output.id);QString reason;if(!before)return false;
        const bool changed=output.enabled!=before->enabled||output.position!=before->position||output.currentMode!=before->currentMode||output.scale!=before->scale||output.rotation!=before->rotation||output.priority!=before->priority;
        if(changed&&!canEdit(baseline,output.id,reason)){error=reason;return false;}
        if(output.followPreferredMode!=before->followPreferredMode||output.replicationSource!=before->replicationSource){error=QStringLiteral("Automatic preferred-mode and mirroring policies cannot be changed by this editor.");return false;}
        if(output.currentMode!=before->currentMode&&before->followPreferredMode){error=QStringLiteral("This output follows its preferred mode automatically. Fixed-mode selection is unavailable through the installed tool.");return false;}
        if(output.scale!=before->scale&&!canScale(baseline)){error=QStringLiteral("Per-output scaling is unavailable on this native backend.");return false;}
        if(output.scale!=before->scale&&(!writableScale(output.scale)||!writableScale(before->scale))) {
            error=QStringLiteral("This scale change cannot be safely written and restored exactly through the native display protocol. The saved scale will be preserved during unrelated edits.");
            return false;
        }
        if(!output.enabled)continue;++enabled;
        const DisplayMode *mode=nullptr;for(const auto &candidate:output.modes)if(candidate.id==output.currentMode)mode=&candidate;
        if(!mode||!output.connected||output.position.x()<0||output.position.y()<0){error=QStringLiteral("Every enabled output needs an advertised mode and a valid arrangement position.");return false;}
        const bool quarter=output.rotation==2||output.rotation==8||output.rotation==32||output.rotation==128;
        const double factor=canScale(baseline)?output.scale:1;
        const QSizeF size(std::ceil((quarter?mode->height:mode->width)/factor),std::ceil((quarter?mode->width:mode->height)/factor));
        const QRectF rect(output.position,size);bounds=bounds.isNull()?rect:bounds.united(rect);
    }
    if(!enabled){error=QStringLiteral("At least one connected output must remain enabled.");return false;}
    const auto screen=baseline.json.value("screen").toObject();const int maximum=screen.value("maxActiveOutputsCount").toInt(64);
    if(enabled>maximum){error=QStringLiteral("This configuration exceeds the native active-output limit.");return false;}
    const auto maxSize=screen.value("maxSize").toObject();if((maxSize.contains("width")&&bounds.width()>maxSize.value("width").toInt())||(maxSize.contains("height")&&bounds.height()>maxSize.value("height").toInt())){error=QStringLiteral("The arrangement exceeds the native screen size limit.");return false;}
    error.clear();return true;
}
bool DisplayProtocol::matchesRequested(const DisplaySnapshot &baseline,const DisplaySnapshot &requested,const DisplaySnapshot &actual){QString error;return canChange(baseline,requested,error)&&same(normalized(requested),actual);}
QStringList DisplayProtocol::changeArguments(const DisplaySnapshot &baseline,const DisplaySnapshot &requested) {
    QString error;if(!canChange(baseline,requested,error))return {};const auto target=normalized(requested);QStringList arguments;
    for(const auto &output:target.outputs){const auto *before=baseline.output(output.id);const auto prefix=QStringLiteral("output.")+output.name+QLatin1Char('.');
        if(before->enabled!=output.enabled)arguments<<prefix+(output.enabled?QStringLiteral("enable"):QStringLiteral("disable"));
        if(before->currentMode!=output.currentMode)arguments<<prefix+QStringLiteral("mode.")+output.currentMode;
        if(before->position!=output.position)arguments<<prefix+QStringLiteral("position.%1,%2").arg(output.position.x()).arg(output.position.y());
        if(before->scale!=output.scale)arguments<<prefix+QStringLiteral("scale.")+QString::number(output.scale,'g',16);
        if(before->rotation!=output.rotation)arguments<<prefix+QStringLiteral("rotation.")+rotationName(output.rotation);
        if(before->priority!=output.priority)arguments<<prefix+QStringLiteral("priority.%1").arg(output.priority);
    }return arguments;
}
bool DisplayProtocol::ownedIntermediate(const DisplaySnapshot &baseline,const DisplaySnapshot &requested,const DisplaySnapshot &actual) {
    const auto target=normalized(requested);if(!baseline.isValid()||!target.isValid()||!actual.isValid()||projection(baseline,false)!=projection(target,false)||projection(baseline,false)!=projection(actual,false))return false;
    if (!ownedPositions(baseline,target,actual)) return false;
    auto partialBase=baseline,partialTarget=target;bool enabledChanged=false;
    for(auto &output:partialBase.outputs){const auto *observed=actual.output(output.id),*desired=target.output(output.id);if(!observed||!desired)return false;enabledChanged|=output.enabled!=desired->enabled;output.enabled=observed->enabled;partialTarget.output(output.id)->enabled=observed->enabled;}
    partialBase=normalized(partialBase);partialTarget=normalized(partialTarget);
    const auto baseValues=projection(baseline,true).value("outputs").toArray(),targetValues=projection(target,true).value("outputs").toArray(),observedValues=projection(actual,true).value("outputs").toArray();
    for(int i=0;i<baseValues.size();++i){const auto before=baseValues[i].toObject(),desired=targetValues[i].toObject(),observed=observedValues[i].toObject();
        for(const auto &key:mutableKeys()) {
            if(key==QLatin1String("pos")) continue; // Proven together by ownedPositions.
            if(observed.value(key)==before.value(key)||observed.value(key)==desired.value(key))continue;
            if(key==QLatin1String("priority")&&enabledChanged){const auto *a=actual.output(before.value("id").toInt()),*p=partialBase.output(before.value("id").toInt()),*q=partialTarget.output(before.value("id").toInt());if(a&&(!a->enabled||(p&&a->priority==p->priority)||(q&&a->priority==q->priority)))continue;}
            return false;
        }
    }return true;
}
bool DisplayProtocol::rollbackRequest(const DisplaySnapshot &baseline,const DisplaySnapshot &requested,const DisplaySnapshot &actual,DisplaySnapshot &restoration,QString &error) {
    restoration={};if(!ownedIntermediate(baseline,requested,actual)){error=QStringLiteral("The display configuration changed outside this transaction. Automatic rollback stopped to preserve it.");return false;}
    restoration=actual;
    for(auto &output:restoration.outputs){const auto *before=baseline.output(output.id);output.enabled=before->enabled;output.currentMode=before->currentMode;output.position=before->position;output.scale=before->scale;output.rotation=before->rotation;output.priority=before->priority;}
    restoration=normalized(restoration);return canChange(actual,restoration,error);
}
QProcessEnvironment DisplayProtocol::doctorEnvironment(){auto environment=QProcessEnvironment::systemEnvironment();if(environment.value("XDG_SESSION_TYPE")==QLatin1String("wayland")&&!environment.value("WAYLAND_DISPLAY").isEmpty())environment.insert("QT_QPA_PLATFORM","wayland");environment.remove("KSCREEN_BACKEND");environment.remove("KSCREEN_BACKEND_INPROCESS");return environment;}
bool DisplayProtocol::matchesMode(const DisplaySnapshot &baseline,const DisplaySnapshot &actual,int id,const QString &mode){auto target=baseline;if(auto *output=target.output(id))output->currentMode=mode;else return false;return matchesRequested(baseline,normalized(target),actual);}
bool DisplayProtocol::canChange(const DisplaySnapshot &snapshot,int id,const QString &mode,QString &error){auto target=snapshot;if(auto *output=target.output(id))output->currentMode=mode;else {error=QStringLiteral("The output is unavailable.");return false;}return canChange(snapshot,normalized(target),error);}
QString DisplayProtocol::modeArgument(int id,const QString &mode){return id>0&&token(mode)?QStringLiteral("output.%1.mode.%2").arg(id).arg(mode):QString();}
