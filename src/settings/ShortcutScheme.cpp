#include "ShortcutScheme.h"
#include <KConfig>
#include <KConfigGroup>
#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStringDecoder>
#include <memory>
#include <algorithm>

namespace ShortcutScheme {
namespace {
constexpr qsizetype maxBytes=4*1024*1024, maxLine=65536, maxEntries=20000, maxGroups=4096, maxAlternatives=64;
const QString contextRoot=QStringLiteral("LinuxControl Context Shortcuts");
const QString extensionRoot=QStringLiteral("LinuxControl Scheme");
void issue(QList<Issue> &issues,Severity severity,const QString &code,const QString &message,const ShortcutActionId &id={})
{issues.append({severity,code,message,id});}
bool hasErrors(const QList<Issue> &issues)
{for(const auto &value:issues)if(value.severity==Severity::Error)return true;return false;}
bool identityText(const QString &text)
{if(text.isEmpty()||text.size()>4096)return false;for(const auto c:text)if(c.unicode()<32||c.unicode()==127)return false;return true;}
bool commandId(const QString &id)
{return identityText(id)&&id.endsWith(QLatin1String(".desktop"))&&!id.contains(QLatin1Char('/'))&&!id.contains(QLatin1Char('\\'));}
bool decoded(const QByteArray &raw,QString &out)
{
    QByteArray bytes;bytes.reserve(raw.size());
    for(qsizetype i=0;i<raw.size();++i){
        char c=raw[i];if(c!='\\'){bytes+=c;continue;}if(++i>=raw.size())return false;c=raw[i];
        if(c=='s')bytes+=' ';else if(c=='n')bytes+='\n';else if(c=='t')bytes+='\t';else if(c=='r')bytes+='\r';else if(c=='\\')bytes+='\\';
        // KConfig retains these desktop-file escapes literally.
        else if(c==';'||c==','){bytes+='\\';bytes+=c;}
        else if(c=='x'){
            if(i+2>=raw.size())return false;
            auto hex=[](char value){return (value>='0'&&value<='9')||(value>='a'&&value<='f')||(value>='A'&&value<='F');};
            if(!hex(raw[i+1])||!hex(raw[i+2]))return false;
            bytes+=char(raw.mid(i+1,2).toInt(nullptr,16));i+=2;
        }else return false;
    }
    QStringDecoder utf8(QStringDecoder::Utf8);out=utf8(bytes);return !utf8.hasError()&&!out.contains(QChar::Null);
}
struct RawEntry {QStringList path;QString key;bool localized=false;};
bool preflight(const QByteArray &bytes,QList<RawEntry> &entries,QList<Issue> &issues)
{
    if(bytes.isEmpty()||bytes.size()>maxBytes){issue(issues,Severity::Error,QStringLiteral("Size"),QObject::tr("A shortcut scheme must contain between 1 byte and 4 MiB."));return false;}
    QStringDecoder utf8(QStringDecoder::Utf8);const QString validated=utf8(bytes);Q_UNUSED(validated);if(utf8.hasError()||bytes.contains('\0')){issue(issues,Severity::Error,QStringLiteral("Encoding"),QObject::tr("Shortcut schemes must be valid UTF-8 without NUL bytes."));return false;}
    QStringList path;QSet<QStringList> groups;QMap<QStringList,QSet<QString>> keys;int lineNo=0;
    for(auto line:bytes.split('\n')){
        ++lineNo;
        auto fail=[&](const QString &message){issue(issues,Severity::Error,QStringLiteral("Syntax"),QObject::tr("Line %1: %2").arg(lineNo).arg(message));};
        if(line.size()>maxLine){fail(QObject::tr("the line is too long."));return false;}
        line=line.trimmed();if(line.isEmpty()||line.startsWith('#'))continue;
        if(line.startsWith('[')){
            path.clear();qsizetype offset=0;
            while(offset<line.size()){
                if(line[offset]!='['){fail(QObject::tr("invalid group header."));return false;}
                const auto close=line.indexOf(']',offset+1);if(close<0){fail(QObject::tr("unterminated group header."));return false;}
                const auto raw=line.mid(offset+1,close-offset-1);QString part;
                if(raw.startsWith('$')||!decoded(raw,part)||!identityText(part)){fail(QObject::tr("invalid, escaped, or policy-bearing group name."));return false;}
                path<<part;offset=close+1;
            }
            if(path.size()>3){fail(QObject::tr("unsupported group nesting."));return false;}
            if(groups.contains(path)){fail(QObject::tr("duplicate or ambiguous group header."));return false;}
            groups.insert(path);if(groups.size()>maxGroups){fail(QObject::tr("too many groups."));return false;}continue;
        }
        const auto equals=line.indexOf('=');if(path.isEmpty()||equals<1){fail(QObject::tr("entries need a named group and an equals sign."));return false;}
        const auto rawKey=line.left(equals).trimmed();QString key,value;bool localized=false;
        if(rawKey.contains('[')||rawKey.contains(']')){
            const auto literal=QString::fromUtf8(rawKey);
            const bool metadata=path==QStringList{QStringLiteral("Settings")}||(path.size()==2&&path[0]==QLatin1String("Custom Commands")&&commandId(path[1]));
            if(!metadata||!QRegularExpression(QStringLiteral("^Name\\[[A-Za-z0-9_@.-]+\\]$")).match(literal).hasMatch()){fail(QObject::tr("localized bindings and policy/expansion flags are unsupported."));return false;}
            key=literal;localized=true;
        }else if(!decoded(rawKey,key)||!identityText(key)){fail(QObject::tr("invalid shortcut entry name."));return false;}
        if(!decoded(line.mid(equals+1).trimmed(),value)){fail(QObject::tr("invalid value escaping."));return false;}
        if(keys[path].contains(key)){fail(QObject::tr("duplicate or ambiguous entry %1.").arg(key));return false;}
        keys[path].insert(key);entries.append({path,key,localized});if(entries.size()>maxEntries){fail(QObject::tr("too many entries."));return false;}
    }
    return true;
}
bool sequenceList(const QString &text,QList<QKeySequence> &keys,QString &error)
{
    keys.clear();if(text.isEmpty()||text.compare(QLatin1String("none"),Qt::CaseInsensitive)==0)return true;
    const auto alternatives=text.split(QStringLiteral("; "),Qt::KeepEmptyParts);
    if(alternatives.size()>maxAlternatives){error=QObject::tr("At most 64 shortcut alternatives are supported.");return false;}
    QSet<QKeySequence> seen;
    for(const auto &alternative:alternatives){
        if(alternative.trimmed().isEmpty()){error=QObject::tr("An empty alternative must be expressed as an empty binding, not mixed with keys.");return false;}
        // Follow Qt's comma-key separator rules, while enforcing its four-chord limit
        // before QKeySequence::fromString could silently discard the fifth chord.
        QString remaining=alternative;int chords=0;
        while(!remaining.isEmpty()){
            auto comma=remaining.indexOf(QLatin1Char(','));qsizetype space=0;
            if(comma>=0){if(comma==remaining.size()-1)comma=-1;else{
                if(remaining[comma+1]==QLatin1Char(','))++comma;
                // A doubled trailing comma is a comma key followed by a
                // missing chord. Reject it before Qt's parser indexes past it.
                if(comma==remaining.size()-1){error=QObject::tr("A shortcut ends with a missing key combination.");return false;}
                if(remaining[comma+1]==QLatin1Char(' ')){space=1;++comma;}
            }}
            const auto part=remaining.left(comma<0?remaining.size():comma-space);
            remaining=remaining.right(comma<0?0:remaining.size()-(comma+1));
            if(part.trimmed().isEmpty()||++chords>4){error=QObject::tr("A shortcut requires one to four nonempty key combinations.");return false;}
        }
        auto key=QKeySequence::fromString(alternative,QKeySequence::PortableText);
        auto valid=[](const QKeySequence &sequence){if(sequence.isEmpty())return false;for(int i=0;i<sequence.count();++i)if(sequence[i].key()==Qt::Key_unknown)return false;return true;};
        if(!valid(key)||key.count()!=chords){error=QObject::tr("Unknown or invalid shortcut sequence: %1").arg(alternative);return false;}
        if(seen.contains(key)){error=QObject::tr("Duplicate shortcut alternative: %1").arg(alternative);return false;}seen.insert(key);keys<<key;
    }
    return true;
}
bool selected(const ShortcutAction &action,const QList<ShortcutActionId> &ids,const QStringList &components)
{return (ids.isEmpty()&&components.isEmpty())||ids.contains(action.id)||components.contains(action.id.component);}
bool validDocument(const Document &document,QList<Issue> &issues)
{
    if(document.bindings.size()>maxEntries||document.commands.size()>maxGroups||document.name.size()>4096){issue(issues,Severity::Error,QStringLiteral("Size"),QObject::tr("The shortcut scheme exceeds the supported size."));return false;}
    QSet<QString> commands;QMap<Scope,QMap<ShortcutActionId,bool>> identities;
    for(const auto &command:document.commands){
        if(!commandId(command.component)||!identityText(command.name)||!identityText(command.exec)||commands.contains(command.component)){issue(issues,Severity::Error,QStringLiteral("Command"),QObject::tr("Invalid or duplicate custom command definition: %1").arg(command.component));return false;}commands.insert(command.component);
    }
    for(const auto &binding:document.bindings){
        if(binding.scope!=Scope::Global&&binding.scope!=Scope::Standard){issue(issues,Severity::Error,QStringLiteral("Scope"),QObject::tr("A shortcut binding has an unknown scope."),binding.id);return false;}
        if(!identityText(binding.id.action)||(binding.scope==Scope::Global&&(!identityText(binding.id.component)||!identityText(binding.id.context)))){issue(issues,Severity::Error,QStringLiteral("Identity"),QObject::tr("A shortcut binding has an invalid identity."),binding.id);return false;}
        if(binding.scope==Scope::Standard&&binding.id.context!=QLatin1String("default")){issue(issues,Severity::Error,QStringLiteral("Context"),QObject::tr("Standard shortcut schemes support only the native default context."),binding.id);return false;}
        auto canonical=binding.id;if(binding.scope==Scope::Standard)canonical={QString{},QStringLiteral("default"),binding.id.action};
        if(identities[binding.scope].contains(canonical)){issue(issues,Severity::Error,QStringLiteral("Duplicate"),QObject::tr("Duplicate or ambiguous shortcut identity."),binding.id);return false;}identities[binding.scope].insert(canonical,true);
        QList<QKeySequence> decodedKeys;QString error;
        if(!sequenceList(QKeySequence::listToString(binding.keys,QKeySequence::PortableText),decodedKeys,error)||decodedKeys!=binding.keys){issue(issues,Severity::Error,QStringLiteral("Sequence"),error.isEmpty()?QObject::tr("A shortcut sequence cannot be represented without loss."):error,binding.id);return false;}
    }
    return true;
}
}
ReadResult parse(const QByteArray &input)
{
    ReadResult result;QByteArray bytes=input;if(bytes.startsWith("\xEF\xBB\xBF"))bytes.remove(0,3);
    QList<RawEntry> entries;if(!preflight(bytes,entries,result.issues)){result.error=result.issues.last().message;return result;}
    auto buffer=std::make_shared<QBuffer>();buffer->setData(bytes);buffer->open(QIODevice::ReadOnly);KConfig config(buffer,KConfig::SimpleConfig);
    auto groupFor=[&](const QStringList &path){KConfigGroup group(&config,path.first());for(int i=1;i<path.size();++i)group=group.group(path[i]);return group;};
    result.document.name=config.group(QStringLiteral("Settings")).readEntry(QStringLiteral("Name"));
    const auto version=config.group(extensionRoot).readEntry(QStringLiteral("ContextVersion"),QString{});
    QMap<QString,Command> commands;QMap<Scope,QMap<ShortcutActionId,bool>> identities;
    for(const auto &entry:entries){
        const auto &path=entry.path;if(entry.localized)continue;
        const auto group=groupFor(path);const auto value=group.readEntry(entry.key,QString{});
        if(path==QStringList{QStringLiteral("Settings")}&&entry.key==QLatin1String("Name"))continue;
        if(path==QStringList{extensionRoot}&&entry.key==QLatin1String("ContextVersion")){
            if(value!=QLatin1String("1"))issue(result.issues,Severity::Error,QStringLiteral("ExtensionVersion"),QObject::tr("Unsupported explicit context extension version."));continue;
        }
        if(path.size()==2&&path[0]==QLatin1String("Custom Commands")&&path[1]!=QLatin1String("Global Shortcuts")){
            if(entry.key!=QLatin1String("Exec")&&entry.key!=QLatin1String("Name")){issue(result.issues,Severity::Error,QStringLiteral("CommandKey"),QObject::tr("Unknown custom command field: %1").arg(entry.key));continue;}
            auto &command=commands[path[1]];command.component=path[1];if(entry.key==QLatin1String("Exec"))command.exec=value;command.name=group.readEntry(QStringLiteral("Name"));continue;
        }
        Binding binding;
        if(path.size()==2&&path[1]==QLatin1String("Global Shortcuts"))binding.id={path[0],QStringLiteral("default"),entry.key};
        else if(path==QStringList{QStringLiteral("StandardShortcuts")}){binding.scope=Scope::Standard;binding.id={QString{},QStringLiteral("default"),entry.key};}
        else if(path.size()==3&&path[0]==contextRoot){
            if(version!=QLatin1String("1")||path[2]==QLatin1String("default")){issue(result.issues,Severity::Error,QStringLiteral("Context"),QObject::tr("Nondefault contexts require explicit context extension version 1; default aliases are forbidden."));continue;}
            binding.id={path[1],path[2],entry.key};
        }else{issue(result.issues,Severity::Error,QStringLiteral("UnknownGroup"),QObject::tr("Unsupported scheme group or metadata key: %1 / %2").arg(path.join(QLatin1Char('/')),entry.key));continue;}
        QString error;if(!sequenceList(value,binding.keys,error)){issue(result.issues,Severity::Error,QStringLiteral("Sequence"),error,binding.id);continue;}
        if(identities[binding.scope].contains(binding.id)){issue(result.issues,Severity::Error,QStringLiteral("Duplicate"),QObject::tr("Duplicate shortcut identity across scheme sections."),binding.id);continue;}identities[binding.scope].insert(binding.id,true);result.document.bindings<<binding;
    }
    for(auto it=commands.cbegin();it!=commands.cend();++it)result.document.commands<<it.value();
    if(!result.document.name.isEmpty()&&!identityText(result.document.name))issue(result.issues,Severity::Error,QStringLiteral("Name"),QObject::tr("Invalid shortcut scheme display name."));
    if(!hasErrors(result.issues))validDocument(result.document,result.issues);
    if(hasErrors(result.issues)){for(const auto &value:result.issues)if(value.severity==Severity::Error){result.error=value.message;break;}return result;}
    if(result.document.bindings.isEmpty()&&result.document.commands.isEmpty()){result.error=QObject::tr("The file contains no shortcut bindings or custom command definitions.");issue(result.issues,Severity::Error,QStringLiteral("Empty"),result.error);return result;}
    result.success=true;return result;
}
ReadResult readFile(const QString &path)
{
    ReadResult result;const QFileInfo info(path);QFile file(path);
    if(!info.isAbsolute()||!info.isFile()||info.suffix().compare(QLatin1String("kksrc"),Qt::CaseInsensitive)!=0||!file.open(QIODevice::ReadOnly)||file.size()>maxBytes){result.error=QObject::tr("Select a readable local .kksrc file of at most 4 MiB.");issue(result.issues,Severity::Error,QStringLiteral("File"),result.error);return result;}
    const auto bytes=file.read(maxBytes+1);if(file.error()!=QFileDevice::NoError||bytes.size()>maxBytes){result.error=QObject::tr("Reading the shortcut scheme failed or exceeded its size limit.");return result;}return parse(bytes);
}
ImportPlan planImport(const Document &document,const ShortcutsSnapshot &globals,const ShortcutsSnapshot &standards,const QMap<QString,QString> &commandMigrations)
{
    ImportPlan plan;plan.migrations=commandMigrations;
    if(!validDocument(document,plan.issues)){plan.error=plan.issues.last().message;return plan;}
    QMap<ShortcutActionId,const ShortcutAction *> globalIndex;QMap<QString,const ShortcutAction *> standardIndex;
    QMap<QString,QSet<QString>> globalContexts;
    QMap<QPair<QString,QString>,QSet<QString>> actionContexts;
    for(const auto &a:globals.actions){
        if(globalIndex.contains(a.id)){issue(plan.issues,Severity::Error,QStringLiteral("AmbiguousSnapshot"),QObject::tr("The global snapshot contains duplicate action identities."),a.id);continue;}globalIndex.insert(a.id,&a);
        globalContexts[a.id.component].insert(a.id.context);
        actionContexts[{a.id.component,a.id.action}].insert(a.id.context);
    }
    for(const auto &a:standards.actions){
        if(standardIndex.contains(a.id.action)){issue(plan.issues,Severity::Error,QStringLiteral("AmbiguousSnapshot"),QObject::tr("The standard snapshot contains ambiguous action names."),a.id);continue;}standardIndex.insert(a.id.action,&a);
    }
    QSet<QString> sourceCommands,mappedCommands;
    for(const auto &command:document.commands)sourceCommands.insert(command.component);
    for(auto it=commandMigrations.cbegin();it!=commandMigrations.cend();++it)
        if(!sourceCommands.contains(it.key())||!commandId(it.value()))issue(plan.issues,Severity::Error,QStringLiteral("Migration"),QObject::tr("Custom command migration must map a scheme definition to a valid desktop component: %1").arg(it.key()));
    for(auto command:document.commands){
        command.component=commandMigrations.value(command.component,command.component);
        if(mappedCommands.contains(command.component))issue(plan.issues,Severity::Error,QStringLiteral("AmbiguousMigration"),QObject::tr("More than one command definition maps to %1.").arg(command.component));
        mappedCommands.insert(command.component);plan.commands<<command;
        issue(plan.issues,Severity::Warning,QStringLiteral("CommandDefinitionReview"),QObject::tr("Custom command %1 is retained for explicit definition review; importing bindings does not create or execute it.").arg(command.name),{command.component,QStringLiteral("default"),QStringLiteral("_launch")});
    }
    QMap<Scope,QMap<ShortcutActionId,bool>> seen;
    for(auto binding:document.bindings){
        const ShortcutAction *action=nullptr;
        if(binding.scope==Scope::Global){
            binding.id.component=commandMigrations.value(binding.id.component,binding.id.component);action=globalIndex.value(binding.id,nullptr);
        }else{
            action=standardIndex.value(binding.id.action,nullptr);
            if(action)binding.id=action->id;
        }
        if(seen[binding.scope].contains(binding.id)){issue(plan.issues,Severity::Error,QStringLiteral("AmbiguousMigration"),QObject::tr("More than one imported entry resolves to the same action."),binding.id);continue;}seen[binding.scope].insert(binding.id,true);
        const auto &snapshot=binding.scope==Scope::Global?globals:standards;
        if(!action){
            QString code=QStringLiteral("UnknownAction"),message=QObject::tr("No installed action matches this imported shortcut identity.");
            if(binding.scope==Scope::Global){
                const bool component=globalContexts.contains(binding.id.component);
                const bool context=globalContexts.value(binding.id.component).contains(binding.id.context);
                const auto contexts=actionContexts.value({binding.id.component,binding.id.action});
                const bool otherContext=!contexts.isEmpty()&&!contexts.contains(binding.id.context);
                if(mappedCommands.contains(binding.id.component)){code=QStringLiteral("PendingCommand");message=QObject::tr("This custom command definition and its binding are retained. Register it through the reviewed custom-command Apply, refresh, then explicitly review its bindings again.");}
                else if(!component){code=QStringLiteral("UnknownComponent");message=QObject::tr("The imported component is not registered on this desktop.");}
                else if(!context||otherContext){code=QStringLiteral("UnknownContext");message=QObject::tr("The imported context does not match an installed action. No context is guessed or activated.");}
            }
            plan.unresolved<<binding;issue(plan.issues,Severity::Warning,code,message,binding.id);continue;
        }
        if(!snapshot.available||!snapshot.writable||!action->available){
            plan.unresolved<<binding;issue(plan.issues,Severity::Warning,QStringLiteral("ReadOnly"),QObject::tr("This imported binding is currently unavailable or read-only."),binding.id);continue;
        }
        (binding.scope==Scope::Global?plan.globals:plan.standards).insert(action->id,binding.keys);
    }
    if(hasErrors(plan.issues)){for(const auto &value:plan.issues)if(value.severity==Severity::Error){plan.error=value.message;break;}return plan;}
    plan.valid=true;return plan;
}
ExportResult exportFile(const QString &path,const QString &name,const ShortcutsSnapshot &globals,const ShortcutsSnapshot &standards,const Selection &selection,const QList<Command> &commands)
{
    ExportResult result;const QFileInfo target(path);
    if(!target.isAbsolute()||target.suffix().compare(QLatin1String("kksrc"),Qt::CaseInsensitive)!=0||target.isSymLink()||(target.exists()&&!target.isFile())){result.error=QObject::tr("Choose a local regular .kksrc destination; symbolic links and native preference filenames are not export targets.");return result;}
    Document document;document.name=name;
    auto collect=[&](Scope scope,const ShortcutsSnapshot &snapshot,bool enabled,const QList<ShortcutActionId> &ids,const QStringList &components){
        if(!enabled)return;
        QSet<QString> availableComponents;QMap<ShortcutActionId,bool> availableActions;
        for(const auto &a:snapshot.actions){availableComponents.insert(a.id.component);availableActions.insert(a.id,true);if(selected(a,ids,components))document.bindings.append({scope,a.id,a.current});}
        for(const auto &id:ids)if(!availableActions.contains(id))issue(result.issues,Severity::Error,QStringLiteral("Selection"),QObject::tr("An explicitly selected export action is no longer in the staged snapshot."),id);
        for(const auto &component:components){
            bool command=false;if(scope==Scope::Global)for(const auto &c:commands)if(c.component==component)command=true;
            if(!availableComponents.contains(component)&&!command)issue(result.issues,Severity::Error,QStringLiteral("Selection"),QObject::tr("An explicitly selected export component is unavailable: %1").arg(component));
        }
    };
    collect(Scope::Global,globals,selection.globals,selection.globalActions,selection.globalComponents);
    collect(Scope::Standard,standards,selection.standards,selection.standardActions,selection.standardComponents);
    if(selection.globals)for(const auto &command:commands){
        bool include=selection.globalActions.isEmpty()&&selection.globalComponents.isEmpty();
        if(selection.globalComponents.contains(command.component))include=true;
        for(const auto &id:selection.globalActions)if(id.component==command.component)include=true;
        if(include)document.commands<<command;
    }
    if(!name.isEmpty()&&!identityText(name))issue(result.issues,Severity::Error,QStringLiteral("Name"),QObject::tr("The shortcut scheme display name is invalid."));
    if(document.bindings.isEmpty()&&document.commands.isEmpty())issue(result.issues,Severity::Error,QStringLiteral("Selection"),QObject::tr("Select at least one shortcut action or custom command definition to export."));
    if(!hasErrors(result.issues))validDocument(document,result.issues);
    if(hasErrors(result.issues)){for(const auto &value:result.issues)if(value.severity==Severity::Error){result.error=value.message;break;}return result;}
    auto buffer=std::make_shared<QBuffer>();buffer->open(QIODevice::ReadWrite);
    {
        KConfig config(buffer,KConfig::SimpleConfig);if(!name.isEmpty())config.group(QStringLiteral("Settings")).writeEntry(QStringLiteral("Name"),name);
        bool contexts=false;QSet<QString> standardNames;
        for(const auto &binding:document.bindings){
            KConfigGroup group;
            if(binding.scope==Scope::Standard){
                if(binding.id.context!=QLatin1String("default")||standardNames.contains(binding.id.action)){result.error=QObject::tr("Standard shortcut export requires unique native action names in the default context.");return result;}standardNames.insert(binding.id.action);group=config.group(QStringLiteral("StandardShortcuts"));
            }else if(binding.id.context==QLatin1String("default"))group=config.group(binding.id.component).group(QStringLiteral("Global Shortcuts"));
            else {contexts=true;group=config.group(contextRoot).group(binding.id.component).group(binding.id.context);}
            group.writeEntry(binding.id.action,QKeySequence::listToString(binding.keys,QKeySequence::PortableText));
        }
        if(contexts){config.group(extensionRoot).writeEntry(QStringLiteral("ContextVersion"),QStringLiteral("1"));issue(result.issues,Severity::Warning,QStringLiteral("ContextExtension"),QObject::tr("Nondefault contexts are preserved in an explicit versioned extension. Native Plasma import reads the compatible default sections and ignores this extension."));}
        for(const auto &command:document.commands){auto group=config.group(QStringLiteral("Custom Commands")).group(command.component);group.writeEntry(QStringLiteral("Exec"),command.exec);group.writeEntry(QStringLiteral("Name"),command.name);}
        if(!config.sync()){result.error=QObject::tr("Serializing the native shortcut scheme failed.");return result;}
    }
    const auto bytes=buffer->data();if(bytes.size()>maxBytes){result.error=QObject::tr("The exported scheme exceeds 4 MiB.");return result;}
    const auto check=parse(bytes);bool equivalent=check.success&&check.document.name==document.name&&check.document.bindings.size()==document.bindings.size()&&check.document.commands.size()==document.commands.size();
    QMap<Scope,QMap<ShortcutActionId,QList<QKeySequence>>> checkedBindings;
    QMap<QString,Command> checkedCommands;
    for(const auto &binding:check.document.bindings)checkedBindings[binding.scope].insert(binding.id,binding.keys);
    for(const auto &command:check.document.commands)checkedCommands.insert(command.component,command);
    for(const auto &binding:document.bindings){
        const auto id=binding.scope==Scope::Standard?ShortcutActionId{QString{},QStringLiteral("default"),binding.id.action}:binding.id;
        equivalent=equivalent&&checkedBindings.value(binding.scope).contains(id)&&checkedBindings.value(binding.scope).value(id)==binding.keys;
    }
    for(const auto &command:document.commands){const auto actual=checkedCommands.value(command.component);equivalent=equivalent&&actual.component==command.component&&actual.name==command.name&&actual.exec==command.exec;}
    if(!equivalent){result.error=QObject::tr("The serialized shortcut scheme failed independent codec verification: %1").arg(check.error);return result;}
    QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){result.error=QObject::tr("The shortcut scheme could not be atomically exported: %1").arg(file.errorString());return result;}
    result.success=true;return result;
}
QList<BuiltIn> builtInSchemes(const QStringList &dataRoots)
{
    QList<BuiltIn> result;QStringList roots=dataRoots.isEmpty()?QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation):dataRoots;QSet<QString> seen;
    for(const auto &root:roots){
        if(!QDir::isAbsolutePath(root))continue;QDir directory(root+QStringLiteral("/kcmkeys"));
        QDirIterator files(directory.absolutePath(),{QStringLiteral("*.kksrc")},QDir::Files,QDirIterator::NoIteratorFlags);
        while(files.hasNext()){
            const auto path=files.next();if(seen.contains(path))continue;seen.insert(path);
            if(result.size()>=maxGroups){BuiltIn limit;limit.name=QObject::tr("Shortcut scheme discovery limit reached");issue(limit.issues,Severity::Error,QStringLiteral("Limit"),QObject::tr("More than 4096 built-in shortcut schemes were found. Narrow the data roots before continuing."));result<<limit;return result;}
            const auto read=readFile(path);BuiltIn value;value.path=path;value.name=read.document.name.isEmpty()?QFileInfo(path).fileName():read.document.name;value.issues=read.issues;
            if(!read.success&&value.issues.isEmpty())issue(value.issues,Severity::Error,QStringLiteral("File"),read.error);result<<value;
        }
    }
    std::sort(result.begin(),result.end(),[](const auto &a,const auto &b){return a.name==b.name?a.path<b.path:a.name<b.name;});return result;
}
}
