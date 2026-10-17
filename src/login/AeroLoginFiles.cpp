#include "AeroLoginFiles.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryFile>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace AeroLoginFiles {
namespace {
constexpr qint64 ConfigLimit=2*1024*1024, JournalLimit=64*1024*1024;
const QString Theme=QStringLiteral("/usr/share/sddm/themes/sddm-theme-mod");
const QString User=Theme+QStringLiteral("/theme.conf.user"), Background=Theme+QStringLiteral("/background"), Default=Theme+QStringLiteral("/default-background");
const QString Journal=Theme+QStringLiteral("/.controlpanel-aero-login.journal"), Lock=Theme+QStringLiteral("/.controlpanel-aero-login.lock");
const QString Main=Theme+QStringLiteral("/Main.qml"), Metadata=Theme+QStringLiteral("/metadata.desktop");
const QString Auth=QStringLiteral("/usr/share/plasma/shells/io.gitgud.wackyideas.desktop/contents/lockscreen/AuthUI.qml");
const QString Provider=QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/qml/aeroshell/utils/libaeroshellutilsplugin.so");
const QString Selection=QStringLiteral("/etc/sddm.conf.d/kde_settings.conf");
QByteArray hash(const QByteArray &bytes){return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);}
QString path(const Context &c,const QString &relative){return c.root==QLatin1String("/")?relative:QDir(c.root).filePath(relative.mid(1));}
bool contextValid(const Context &c){return QFileInfo(c.root).isAbsolute()&&QDir::cleanPath(c.root)==c.root&&(c.fixture||c.root==QLatin1String("/")&&c.expectedUid==0&&c.approvedHashes.isEmpty()&&!c.fail);}
struct File {
    QString name,error;bool present=false,ok=true;QByteArray bytes,digest;struct stat st{};
};
File load(const Context &c,const QString &name,qint64 limit)
{
    File f;f.name=name;const auto encoded=QFile::encodeName(path(c,name));
    if(::lstat(encoded.constData(),&f.st)!=0){if(errno==ENOENT)return f;f.ok=false;f.error=QStringLiteral("Cannot inspect %1.").arg(name);return f;}
    f.present=true;
    if(!S_ISREG(f.st.st_mode)||f.st.st_size<0||f.st.st_size>limit){f.ok=false;f.error=QStringLiteral("Not a bounded regular file: %1.").arg(name);return f;}
    const int fd=::open(encoded.constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){f.ok=false;f.error=QStringLiteral("Cannot read %1.").arg(name);return f;}
    struct stat before{},after{};bool valid=::fstat(fd,&before)==0&&S_ISREG(before.st_mode)&&before.st_dev==f.st.st_dev&&before.st_ino==f.st.st_ino;
    QByteArray bytes;char block[16384];
    while(valid){const auto count=::read(fd,block,sizeof(block));if(count<0){if(errno==EINTR)continue;valid=false;break;}if(!count)break;bytes.append(block,int(count));if(bytes.size()>limit){valid=false;break;}}
    valid=valid&&::fstat(fd,&after)==0&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size
        &&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec;
    ::close(fd);f.ok=valid;if(valid){f.bytes=bytes;f.digest=hash(bytes);f.st=after;}else f.error=QStringLiteral("File changed or failed its bounded read: %1.").arg(name);return f;
}
QByteArray identity(const File &f)
{
    QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);
    stream<<f.name<<f.present<<f.ok<<f.digest;
    if(f.present)stream<<quint64(f.st.st_dev)<<quint64(f.st.st_ino)<<quint32(f.st.st_uid)<<quint32(f.st.st_gid)<<quint32(f.st.st_mode)
        <<qint64(f.st.st_size)<<qint64(f.st.st_mtim.tv_sec)<<qint64(f.st.st_mtim.tv_nsec)<<qint64(f.st.st_ctim.tv_sec)<<qint64(f.st.st_ctim.tv_nsec);
    return bytes;
}
bool trusted(const Context &c,const QString &name,bool missing=true)
{
    QString current=path(c,name);bool leaf=true;
    for(;;){struct stat st{};const auto encoded=QFile::encodeName(current);
        if(::lstat(encoded.constData(),&st)!=0){if(!leaf||!missing||errno!=ENOENT)return false;}
        else if(S_ISLNK(st.st_mode)||st.st_uid!=c.expectedUid||(st.st_mode&0022)||(st.st_mode&07000)||(leaf?!S_ISREG(st.st_mode):!S_ISDIR(st.st_mode)))return false;
        if(current==c.root)return true;
        const auto parent=QFileInfo(current).absolutePath();if(parent==current)return c.root==QLatin1String("/");current=parent;leaf=false;
    }
}
QMap<QString,QVariant> parseSafe(const QByteArray &bytes,bool &ok)
{
    QTemporaryFile scratch(QDir::tempPath()+QStringLiteral("/control-aero-ini-XXXXXX"));
    if(!scratch.open()||scratch.write(bytes)!=bytes.size()||!scratch.flush()){ok=false;return {};}
    QSettings settings(scratch.fileName(),QSettings::IniFormat);settings.setFallbacksEnabled(false);
    QMap<QString,QVariant> values;for(const auto &key:settings.allKeys())values.insert(key,settings.value(key));
    ok=settings.status()==QSettings::NoError;return values;
}
QByteArray syntaxPrefix(QByteArray bytes,bool first)
{
    // For line/section classification Qt removes a BOM only at absolute byte
    // zero, then skips these four Space bytes. Key UTF-8 decoding can also
    // absorb a BOM after key trimming. VT/FF and '#' are ordinary prefixes.
    if(first&&bytes.startsWith("\xef\xbb\xbf"))bytes.remove(0,3);
    int pos=0;while(pos<bytes.size()&&(bytes[pos]==' '||bytes[pos]=='\t'||bytes[pos]=='\n'||bytes[pos]=='\r'))++pos;
    return bytes.mid(pos);
}
struct Line {QByteArray bytes,prefix,key,value,comment;bool assignment=false;};
QList<Line> lines(const QByteArray &bytes)
{
    QList<Line> out;int start=0;
    while(start<bytes.size()){
        bool quoted=false;int end=start,equal=-1,comment=-1;
        for(;end<bytes.size();++end){const char ch=bytes[end];
            if(ch=='\\'){if(end+1<bytes.size()){const char escaped=bytes[++end];if(end+1<bytes.size()&&((escaped=='\r'&&bytes[end+1]=='\n')||(escaped=='\n'&&bytes[end+1]=='\r')))++end;}continue;}
            if(ch=='"'){quoted=!quoted;continue;}
            if(!quoted&&ch==';'){comment=end;while(end<bytes.size()&&bytes[end]!='\n'&&bytes[end]!='\r')++end;if(end<bytes.size()){const char first=bytes[end++];if(end<bytes.size()&&((first=='\r'&&bytes[end]=='\n')||(first=='\n'&&bytes[end]=='\r')))++end;}break;}
            if(!quoted&&ch=='='&&equal<0)equal=end;
            if(!quoted&&(ch=='\n'||ch=='\r')){const char first=ch;++end;if(end<bytes.size()&&((first=='\r'&&bytes[end]=='\n')||(first=='\n'&&bytes[end]=='\r')))++end;break;}
        }
        if(end<=start)break;Line l;l.bytes=bytes.mid(start,end-start);l.prefix=syntaxPrefix(l.bytes,start==0);
        if(equal>=start&&!l.prefix.startsWith(';')&&!l.prefix.startsWith('[')){
            l.assignment=true;l.key=bytes.mid(start,equal-start);if(start==0&&l.key.startsWith("\xef\xbb\xbf"))l.key.remove(0,3);
            l.value=bytes.mid(equal+1,(comment>=0?comment:end)-equal-1).trimmed();if(comment>=0)l.comment=bytes.mid(comment,end-comment);
        }
        out<<l;start=end;
    }
    return out;
}
QMap<QString,QVariant> ini(const QByteArray &bytes,bool &ok)
{
    // The real Qt INI parser handles key aliases. It NEVER sees opaque values:
    // every captured RHS is first represented as plain hexadecimal text.
    QByteArray safe;for(const auto &line:lines(bytes))safe+=line.assignment?line.key+"=hex_"+line.value.toHex()+'\n':line.bytes;
    const auto encoded=parseSafe(safe,ok);QMap<QString,QVariant> result;
    for(auto it=encoded.cbegin();it!=encoded.cend();++it){
        const auto text=it.value().toString().toLatin1();if(!text.startsWith("hex_")){ok=false;continue;}
        const auto raw=QByteArray::fromHex(text.mid(4));bool owned=false;for(const auto key:AeroLoginTypes::keys())owned|=it.key()==AeroLoginTypes::keyName(key);
        if(!owned){result[it.key()]=raw;continue;}
        // @Variant and backslash-generated type prefixes are not primitives and
        // must not trigger QDataStream/image/custom-type deserialization.
        if(raw.contains('@')||raw.contains('\\')||raw.contains('\n')||raw.contains('\r')){result[it.key()]=QStringLiteral("!opaque:")+QString::fromLatin1(raw.toHex());continue;}
        bool scalarOk=true;const auto scalar=parseSafe("[General]\nvalue="+raw+'\n',scalarOk);
        if(!scalarOk){ok=false;continue;}result[it.key()]=scalar.value(QStringLiteral("value"));
    }
    return result;
}
QMap<QString,QByteArray> approved(const Context &c)
{
    if(c.fixture)return c.approvedHashes;
    return {{Main,"b88b2d9c79ba63c9750c27f40d9418f888ec11132f95056d4d6444e2fcba626e"},
        {Metadata,"3aa900650b9bcff332991fff40498ecaccb9511d4800532394c8afa9efff850f"},{Auth,"fc37b26c1aba9f8d22da573a8b5389bf680dbd9083b5cd1917f2070264a66e14"},
        {Provider,"0c6cb27868f7aea8b62a5ebe0920343b506525a0c3c92e4047636424cdcb16f4"},
        {Default,"1db478b26b9a8eb5565a6b96354bd7252fed16a02d3cc4a59fa1159106b622ef"}};
}
QStringList staticNames(){return {Main,Metadata,Auth,Provider,Selection,Theme+QStringLiteral("/theme.conf"),Default};}
QStringList dataNames(){auto result=staticNames();result<<User<<Background;return result;}
qint64 limit(const QString &name){return name==Background||name==Default?AeroLoginTypes::MaximumImageBytes:name==Provider?32*1024*1024:ConfigLimit;}
QMap<QString,File> files(const Context &c)
{QMap<QString,File> result;for(const auto &name:dataNames())result.insert(name,load(c,name,limit(name)));return result;}
QByteArray fingerprint(const QMap<QString,File> &all,bool staticOnly=false)
{QByteArray result;for(const auto &name:staticOnly?staticNames():dataNames())result+=identity(all.value(name));return hash(result);}
AeroLoginAsset asset(const File &f){AeroLoginAsset a;a.present=f.present;a.readable=f.present&&f.ok;a.size=f.st.st_size;a.bytes=f.bytes;a.digest=f.digest;a.reason=f.error;return a;}
bool fail(const Context &c,FailurePoint point){return c.fixture&&c.fail&&c.fail(point);}
struct State {bool present=false;QByteArray bytes,digest;quint32 mode=0644,uid=0,gid=0;};
State state(const File &f,const Context &c){return {f.present,f.bytes,f.present?hash(f.bytes):QByteArray(),f.present?quint32(f.st.st_mode&0777):0644,f.present?quint32(f.st.st_uid):c.expectedUid,f.present?quint32(f.st.st_gid):quint32(::getegid())};}
bool matches(const File &f,const State &s){return f.ok&&f.present==s.present&&(!s.present||(f.digest==s.digest&&quint32(f.st.st_mode&0777)==s.mode&&f.st.st_uid==s.uid&&f.st.st_gid==s.gid));}
QJsonObject json(const State &s){return {{"present",s.present},{"bytes",QString::fromLatin1(s.bytes.toBase64())},{"digest",QString::fromLatin1(s.digest.toHex())},{"mode",int(s.mode)},{"uid",double(s.uid)},{"gid",double(s.gid)}};}
bool fromJson(const QJsonValue &value,State &s,qint64 maximum)
{
    if(!value.isObject())return false;const auto o=value.toObject();if(o.size()!=6||!o["present"].isBool()||!o["bytes"].isString()||!o["digest"].isString()||!o["mode"].isDouble()||!o["uid"].isDouble()||!o["gid"].isDouble())return false;
    s.present=o["present"].toBool();const auto encoded=o["bytes"].toString().toLatin1();s.bytes=QByteArray::fromBase64(encoded);s.digest=QByteArray::fromHex(o["digest"].toString().toLatin1());
    if(s.bytes.size()>maximum||s.bytes.toBase64()!=encoded)return false;
    const auto mode=o["mode"].toDouble(),uid=o["uid"].toDouble(),gid=o["gid"].toDouble();
    if(mode<0||mode>0777||mode!=int(mode)||uid<0||uid>4294967295.0||uid!=quint32(uid)||gid<0||gid>4294967295.0||gid!=quint32(gid))return false;
    s.mode=quint32(mode);s.uid=quint32(uid);s.gid=quint32(gid);
    return s.present?(s.digest==hash(s.bytes)):(s.bytes.isEmpty()&&s.digest.isEmpty());
}
struct Recovery {bool valid=false;QString error;QByteArray staticRevision;State beforeUser,afterUser,beforeImage,afterImage;};
QByteArray journalBytes(const Recovery &r);
Recovery journal(const File &file,const QMap<QString,File> &all,const Context &c)
{
    Recovery r;if(!file.present||!file.ok)return r;
    const auto o=QJsonDocument::fromJson(file.bytes).object();
    if(o.size()!=6||o.value("version").toInt()!=1||!o.value("staticRevision").isString()
        ||!fromJson(o.value("beforeUser"),r.beforeUser,ConfigLimit)||!fromJson(o.value("afterUser"),r.afterUser,ConfigLimit)
        ||!fromJson(o.value("beforeImage"),r.beforeImage,AeroLoginTypes::MaximumImageBytes)||!fromJson(o.value("afterImage"),r.afterImage,AeroLoginTypes::MaximumImageBytes)){
        r.error=QStringLiteral("The recovery journal is malformed or outside its bounded schema.");return r;
    }
    r.staticRevision=QByteArray::fromHex(o.value("staticRevision").toString().toLatin1());
    if(!trusted(c,Journal,false)||r.staticRevision!=fingerprint(all,true)||r.beforeUser.uid!=c.expectedUid||r.afterUser.uid!=c.expectedUid||r.beforeImage.uid!=c.expectedUid||r.afterImage.uid!=c.expectedUid
        ||(r.beforeUser.mode&0022)||(r.afterUser.mode&0022)||(r.beforeImage.mode&0022)||(r.afterImage.mode&0022)
        ||(r.beforeUser.present&&!(r.beforeUser.mode&S_IROTH))||(r.afterUser.present&&!(r.afterUser.mode&S_IROTH))||(r.beforeImage.present&&!(r.beforeImage.mode&S_IROTH))||(r.afterImage.present&&!(r.afterImage.mode&S_IROTH))
        ||(!matches(all.value(User),r.beforeUser)&&!matches(all.value(User),r.afterUser))||(!matches(all.value(Background),r.beforeImage)&&!matches(all.value(Background),r.afterImage))){
        r.error=QStringLiteral("Recovery is blocked by changed source files, ownership, or foreign edits. The journal is retained.");return r;
    }
    if(file.bytes!=journalBytes(r)){r.error=QStringLiteral("The journal is not the exact canonical transaction record.");return r;}
    for(const auto &pair:{qMakePair(Theme+QStringLiteral("/.controlpanel-aero-login.stage.config"),qMakePair(r.beforeUser,r.afterUser)),qMakePair(Theme+QStringLiteral("/.controlpanel-aero-login.stage.image"),qMakePair(r.beforeImage,r.afterImage))}){
        const auto staged=load(c,pair.first,AeroLoginTypes::MaximumImageBytes);
        if(staged.present&&(!staged.ok||staged.st.st_uid!=c.expectedUid||(staged.st.st_mode&0022)||(!matches(staged,pair.second.first)&&!matches(staged,pair.second.second)))){r.error=QStringLiteral("A foreign or unsafe staging file prevents recovery.");return r;}
    }
    r.valid=true;return r;
}
AeroLoginSnapshot readImpl(const Context &c,bool ignoreJournal,QString *error)
{
    AeroLoginSnapshot out;if(error)error->clear();
    if(!contextValid(c)){out.reason=QStringLiteral("The filesystem context is outside the fixed approved contract.");if(error)*error=out.reason;return out;}
    const auto all=files(c);bool source=true;
    if(!c.fixture&&::geteuid()==0){
        bool secure=true;for(const auto &name:dataNames())secure=trusted(c,name)&&secure;
        if(!secure){out.reason=QStringLiteral("The privileged reader refuses untrusted source paths before INI parsing.");if(error)*error=out.reason;return out;}
    }
    for(auto it=all.cbegin();it!=all.cend();++it)if(!it->ok){out.issues<<it->error;source=false;}
    const auto hashes=approved(c);
    for(const auto &name:{Main,Metadata,Auth,Provider,Default})if(!all.value(name).present||hashes.value(name).isEmpty()||all.value(name).digest.toHex()!=hashes.value(name)){
        out.issues<<QStringLiteral("An approved Aero source asset is missing or changed: %1.").arg(name);source=false;
    }
    bool selectionOk=true;const auto selection=ini(all.value(Selection).bytes,selectionOk);
    if(!selectionOk||selection.value(QStringLiteral("Theme/Current"),QStringLiteral("sddm-theme-mod")).toString()!=QLatin1String("sddm-theme-mod")){
        out.issues<<QStringLiteral("The active SDDM theme is not the fixed approved Aero target.");source=false;
    }
    bool baseOk=true,userOk=true;const auto base=ini(all.value(Theme+QStringLiteral("/theme.conf")).bytes,baseOk),user=ini(all.value(User).bytes,userOk);
    if(!baseOk||!userOk){source=false;out.issues<<QStringLiteral("The native INI sources contain malformed syntax; no keys will be rewritten.");}
    out.sourceValid=source;bool secure=true;
    for(const auto &name:dataNames())secure=trusted(c,name)&&secure;
    secure=trusted(c,Journal)&&trusted(c,Lock)&&secure;
    for(const auto &name:dataNames())if(all.value(name).present&&!(all.value(name).st.st_mode&S_IROTH)){
        secure=false;out.issues<<QStringLiteral("The shared login asset is not publicly readable; its private bytes will not be copied into a recovery journal: %1.").arg(name);
    }
    // Distinct configuration/assets must never alias a write target.
    QMap<QPair<quint64,quint64>,QString> seen;
    for(auto it=all.cbegin();it!=all.cend();++it)if(it->present&&it->ok){const auto key=qMakePair(quint64(it->st.st_dev),quint64(it->st.st_ino));if(seen.contains(key)){secure=false;out.issues<<QStringLiteral("Fixed source paths alias the same inode: %1 and %2.").arg(seen[key],it.key());}seen[key]=it.key();}
    out.trustedInstallation=secure;out.writable=source&&secure;
    const auto editorPath=all.value(User).present?User:Theme+QStringLiteral("/theme.conf");
    // Native editor primitive bool semantics without constructing its dirty
    // copyFrom path or exposing opaque QVariant values to another parser.
    const auto &editorValues=editorPath==User?user:base;
    for(const auto key:AeroLoginTypes::keys()){
        AeroLoginField field;const auto name=AeroLoginTypes::keyName(key);field.basePresent=base.contains(name);field.localPresent=user.contains(name);
        const auto baseValue=base.value(name);const auto userValue=user.value(name);field.baseRaw=baseValue.toString();field.raw=field.localPresent?userValue.toString():QString();field.userEmpty=field.localPresent&&field.raw.isEmpty();
        field.defaultValue=baseValue.toBool();field.defaultKnown=baseOk&&out.sourceValid&&!baseValue.toString().startsWith(QLatin1String("!opaque:"));
        const auto value=field.localPresent&&!field.userEmpty?userValue:baseValue;field.value=value.toBool();field.known=baseOk&&userOk&&out.sourceValid&&!value.toString().startsWith(QLatin1String("!opaque:"));
        field.canonical=!field.localPresent||field.userEmpty||field.raw==QLatin1String("true")||field.raw==QLatin1String("false");
        field.editorKnown=c.fixture&&(editorPath==User?userOk:baseOk)&&!editorValues.value(name).toString().startsWith(QLatin1String("!opaque:"));
        const auto editorRaw=editorValues.value(name);const auto text=editorRaw.toString();
        field.editorValue=editorValues.contains(name)?text.compare(QLatin1String("false"),Qt::CaseInsensitive)!=0&&text.compare(QLatin1String("no"),Qt::CaseInsensitive)!=0&&text.compare(QLatin1String("off"),Qt::CaseInsensitive)!=0&&text!=QLatin1String("0"):AeroLoginTypes::builtinEditorDefault(key);field.writable=out.writable;
        if(field.raw.startsWith(QLatin1String("!opaque:")))field.raw=QString::fromUtf8(QByteArray::fromHex(field.raw.mid(8).toLatin1()));
        if(field.baseRaw.startsWith(QLatin1String("!opaque:")))field.baseRaw=QString::fromUtf8(QByteArray::fromHex(field.baseRaw.mid(8).toLatin1()));
        if(!field.known)field.reason=QStringLiteral("The approved source or native parser could not be verified.");
        else if(!field.editorKnown)field.reason=QStringLiteral("The SDDM value is known; the native editor client global cascade was not verified.");
        else if(field.editorKnown&&field.editorValue!=field.value)field.reason=QStringLiteral("The native settings editor and SDDM consumer disagree for this saved value.");
        out.fields.insert(key,field);
    }
    out.background=asset(all.value(Background));out.approvedDefault=asset(all.value(Default));
    const auto recoveryFile=ignoreJournal?File{}:load(c,Journal,JournalLimit);
    out.recoveryPending=!ignoreJournal&&recoveryFile.present;
    if(out.recoveryPending){
        const auto recovery=journal(recoveryFile,all,c);bool beforeOk=true;
        const auto beforeValues=recovery.valid?ini(recovery.beforeUser.bytes,beforeOk):QMap<QString,QVariant>();
        out.canRecover=recovery.valid&&beforeOk&&source&&secure;
        if(out.canRecover){
            QStringList review{QStringLiteral("Rollback review (current → restored):")};
            const auto shown=[](bool known,bool value){return !known?QStringLiteral("unknown"):value?QStringLiteral("true"):QStringLiteral("false");};
            for(const auto key:AeroLoginTypes::keys()){
                const auto name=AeroLoginTypes::keyName(key);const bool local=beforeValues.contains(name);const auto raw=beforeValues.value(name).toString();
                const bool inherited=!local||raw.isEmpty();const auto value=inherited?base.value(name):beforeValues.value(name);
                const bool known=!value.toString().startsWith(QLatin1String("!opaque:"));
                QString provenance;
                if(!local)provenance=QStringLiteral("saved override absent; inherits base");
                else if(raw.isEmpty())provenance=QStringLiteral("saved override empty; inherits base");
                else if(raw.startsWith(QLatin1String("!opaque:")))provenance=QStringLiteral("saved override opaque; unverified");
                else {QString spelling;for(const auto ch:raw.left(64))spelling+=ch.isPrint()?ch:QLatin1Char('?');provenance=QStringLiteral("saved override spelling: %1%2").arg(spelling,raw.size()>64?QStringLiteral("…"):QString());}
                const auto current=out.fields.value(key);
                review<<QStringLiteral("%1: %2 → %3 (%4)").arg(name,shown(current.known,current.value),shown(known,value.toBool()),provenance);
            }
            const auto imageDescription=[](bool present,qint64 size,const QByteArray &digest){
                return present?QStringLiteral("present, %1 bytes, SHA-256 %2").arg(size).arg(QString::fromLatin1(digest.toHex()))
                    :QStringLiteral("absent (login uses its blue fallback; Aero lock uses the approved default background)");
            };
            review<<QStringLiteral("Background current: %1").arg(imageDescription(out.background.present,out.background.size,out.background.digest));
            review<<QStringLiteral("Background restored: %1").arg(imageDescription(recovery.beforeImage.present,recovery.beforeImage.bytes.size(),recovery.beforeImage.digest));
            review<<QStringLiteral("Recovery restores saved file bytes only. Runtime reload and login/lock adoption are not verified.");
            out.recoverySummary=review.join(QLatin1Char('\n'));
        }else out.recoverySummary=!beforeOk?QStringLiteral("The saved pre-change INI is malformed; recovery is blocked and the journal is retained."):recovery.error.isEmpty()?QStringLiteral("The recovery journal cannot be read safely."):recovery.error;
        out.writable=false;for(auto &field:out.fields)field.writable=false;
    }
    QByteArray revision=fingerprint(all);if(!ignoreJournal){revision+=identity(recoveryFile);
        for(const auto &name:{Theme+QStringLiteral("/.controlpanel-aero-login.stage.config"),Theme+QStringLiteral("/.controlpanel-aero-login.stage.image")}){
            const auto staged=load(c,name,AeroLoginTypes::MaximumImageBytes);
            if(staged.present&&!out.recoveryPending){out.recoveryPending=true;out.canRecover=false;out.recoverySummary=QStringLiteral("An orphan staging file has no valid journal; no automatic cleanup or writes are permitted.");out.writable=false;for(auto &field:out.fields)field.writable=false;}
            if(out.recoveryPending)revision+=identity(staged);
        }
    }out.revision=hash(revision);
    out.reason=!source?out.issues.join(QLatin1Char('\n')):!secure?QStringLiteral("The fixed Aero installation is not root-owned, immutable against untrusted writers, and free of symlinks/aliases. Administrative installation is required before saving."):out.recoveryPending?out.recoverySummary:QString();
    return out;
}
QString decodeKey(const QByteArray &raw)
{
    const auto text=QString::fromUtf8(raw);QString out;
    for(int i=0;i<text.size();){const auto ch=text[i];if(ch==QLatin1Char('\\')){out+=QLatin1Char('/');++i;continue;}
        if(ch==QLatin1Char('%')){const bool unicode=i+1<text.size()&&text[i+1]==QLatin1Char('U');const int digits=unicode?4:2,start=i+(unicode?2:1);bool ok=false;
            if(start+digits<=text.size()){const auto number=text.mid(start,digits).toUShort(&ok,16);if(ok){out+=QChar(number);i=start+digits;continue;}}}
        out+=ch;++i;
    }return out;
}
QByteArray patch(const QByteArray &original,const AeroLoginSnapshot &request)
{
    QByteArray out;QString section;
    for(const auto &line:lines(original)){
        const auto &trimmed=line.prefix;
        if(trimmed.startsWith('[')){
            const int end=trimmed.indexOf(']');const auto name=end<0?QByteArray():trimmed.mid(1,end-1).trimmed();
            if(name.compare("general",Qt::CaseInsensitive)==0)section.clear();
            else if(name.compare("%general",Qt::CaseInsensitive)==0)section=QString::fromUtf8(name.mid(1))+QLatin1Char('/');
            else section=decodeKey(name)+QLatin1Char('/');
        }
        bool remove=false;if(line.assignment){const auto key=section+decodeKey(line.key.trimmed());for(auto i=request.intents.cbegin();i!=request.intents.cend();++i)remove|=key==AeroLoginTypes::keyName(i.key());}
        if(!remove)out+=line.bytes;else if(!line.comment.isEmpty())out+=line.comment;
    }
    bool appended=false;
    for(auto i=request.intents.cbegin();i!=request.intents.cend();++i)if(i.value()==AeroLoginIntent::Set){
        if(!appended){if(!out.isEmpty()&&!out.endsWith('\n'))out+='\n';out+="[General]\n";appended=true;}
        out+=AeroLoginTypes::keyName(i.key()).toUtf8()+'='+(request.fields.value(i.key()).value?"true":"false")+'\n';
    }
    return out;
}
bool unownedPreserved(const QByteArray &before,const QByteArray &after,const AeroLoginSnapshot &request)
{
    bool a=true,b=true;auto old=ini(before,a),now=ini(after,b);
    for(auto i=request.intents.cbegin();i!=request.intents.cend();++i){old.remove(AeroLoginTypes::keyName(i.key()));now.remove(AeroLoginTypes::keyName(i.key()));}
    return a&&b&&old==now;
}
class HeldLock {
public:
    int fd=-1;
    ~HeldLock(){if(fd>=0){::flock(fd,LOCK_UN);::close(fd);}}
    bool acquire(const Context &c,QString &error){
        if(!trusted(c,Lock)){error=QStringLiteral("The fixed transaction lock is not trusted.");return false;}
        const auto name=QFile::encodeName(path(c,Lock));fd=::open(name.constData(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
        struct stat st{};if(fd<0||::fstat(fd,&st)!=0||!S_ISREG(st.st_mode)||st.st_uid!=c.expectedUid||(st.st_mode&0022)||::flock(fd,LOCK_EX|LOCK_NB)!=0){error=QStringLiteral("Another transaction is active or its fixed lock is unsafe.");return false;}return true;
    }
};
bool syncDirectory(const Context &c)
{
    const auto encoded=QFile::encodeName(path(c,Theme));const int fd=::open(encoded.constData(),O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return false;const bool ok=::fsync(fd)==0;::close(fd);return ok;
}
const QString StageUser=Theme+QStringLiteral("/.controlpanel-aero-login.stage.config"),StageImage=Theme+QStringLiteral("/.controlpanel-aero-login.stage.image");
bool stageFile(const Context &c,const QString &name,const State &state,QString &error,File *captured=nullptr)
{
    if(!state.present){if(captured)*captured=load(c,name,AeroLoginTypes::MaximumImageBytes);return true;}if(!trusted(c,name)){error=QStringLiteral("The fixed staging path is unsafe.");return false;}
    const auto encoded=QFile::encodeName(path(c,name));const int fd=::open(encoded.constData(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){error=QStringLiteral("Cannot exclusively create transaction staging file.");return false;}
    bool ok=true;qsizetype offset=0;while(offset<state.bytes.size()){const auto count=::write(fd,state.bytes.constData()+offset,size_t(state.bytes.size()-offset));if(count<0&&errno==EINTR)continue;if(count<=0){ok=false;break;}offset+=count;}
    if(ok)ok=::fchmod(fd,mode_t(state.mode))==0;
    if(ok&&::geteuid()==0)ok=::fchown(fd,state.uid,state.gid)==0;
    if(ok)ok=::fsync(fd)==0;::close(fd);
    if(!ok){::unlink(encoded.constData());error=QStringLiteral("The staged transaction file could not be written durably.");return false;}
    const auto staged=load(c,name,name==Journal?JournalLimit:AeroLoginTypes::MaximumImageBytes);
    if(!matches(staged,state)){error=QStringLiteral("The staged file changed before its identity could be recorded.");return false;}
    if(captured)*captured=staged;return true;
}
bool removeFile(const Context &c,const QString &name)
{const auto encoded=QFile::encodeName(path(c,name));return ::unlink(encoded.constData())==0||errno==ENOENT;}
bool install(const Context &c,const QString &stage,const QString &target,const State &after,const File &recordedStage,const std::function<bool()> &boundaryCurrent,QString &error)
{
    const auto staged=load(c,stage,AeroLoginTypes::MaximumImageBytes);
    if(!matches(staged,after)||identity(staged)!=identity(recordedStage)||!trusted(c,stage)||!boundaryCurrent()
        ||identity(load(c,stage,AeroLoginTypes::MaximumImageBytes))!=identity(recordedStage)){
        error=QStringLiteral("A staged file, journal, or source changed before installation; foreign state was preserved.");return false;
    }
    bool ok;
    if(after.present){const auto a=QFile::encodeName(path(c,stage)),b=QFile::encodeName(path(c,target));ok=::rename(a.constData(),b.constData())==0;}
    else ok=removeFile(c,target);
    if(ok)ok=syncDirectory(c);if(!ok)error=QStringLiteral("A transaction file replacement or directory fsync failed.");return ok;
}
QByteArray journalBytes(const Recovery &r)
{return QJsonDocument(QJsonObject{{"version",1},{"staticRevision",QString::fromLatin1(r.staticRevision.toHex())},{"beforeUser",json(r.beforeUser)},{"afterUser",json(r.afterUser)},{"beforeImage",json(r.beforeImage)},{"afterImage",json(r.afterImage)}}).toJson(QJsonDocument::Compact);}
bool cleanup(const Context &c,const Recovery &r,QString &error)
{
    const auto record=load(c,Journal,JournalLimit);if(!record.present||!record.ok||record.bytes!=journalBytes(r)||!trusted(c,Journal,false)){error=QStringLiteral("The transaction journal changed; it was not removed.");return false;}
    for(const auto &pair:{qMakePair(StageUser,r.afterUser),qMakePair(StageImage,r.afterImage)}){
        const auto f=load(c,pair.first,limit(pair.first)==ConfigLimit?AeroLoginTypes::MaximumImageBytes:limit(pair.first));
        if(f.present&&(!f.ok||f.digest!=hash(pair.second.bytes)||f.st.st_uid!=c.expectedUid)){error=QStringLiteral("A foreign staging file prevents cleanup; the recovery journal is retained.");return false;}
    }
    if(!removeFile(c,StageUser)||!removeFile(c,StageImage)||!removeFile(c,Journal)||!syncDirectory(c)){error=QStringLiteral("Transaction cleanup could not be verified; inspect recovery before retrying.");return false;}return true;
}
bool rollback(const Context &c,const Recovery &r,QString &error)
{
    const auto record=load(c,Journal,JournalLimit);if(!record.present||!record.ok||record.bytes!=journalBytes(r)||!trusted(c,Journal,false)){error=QStringLiteral("A foreign journal edit prevents rollback.");return false;}
    if(fail(c,FailurePoint::Rollback)){error=QStringLiteral("Injected rollback failure; explicit recovery is required.");return false;}
    const auto current=files(c);
    if(r.staticRevision!=fingerprint(current,true)||(!matches(current.value(User),r.beforeUser)&&!matches(current.value(User),r.afterUser))||(!matches(current.value(Background),r.beforeImage)&&!matches(current.value(Background),r.afterImage))){error=QStringLiteral("A foreign edit prevents rollback. The journal is retained.");return false;}
    // Remove only known remaining staging files, then recreate old states.
    for(const auto &pair:{qMakePair(StageUser,qMakePair(r.beforeUser,r.afterUser)),qMakePair(StageImage,qMakePair(r.beforeImage,r.afterImage))}){
        const auto f=load(c,pair.first,AeroLoginTypes::MaximumImageBytes);if(f.present&&(!f.ok||f.st.st_uid!=c.expectedUid||(!matches(f,pair.second.first)&&!matches(f,pair.second.second)))){error=QStringLiteral("A foreign staging edit prevents rollback.");return false;}
        if(!removeFile(c,pair.first)){error=QStringLiteral("Cannot clear a known staging file for rollback.");return false;}
    }
    auto expected=current;
    for(const auto &target:{User,Background}){
        const bool user=target==User;const auto &before=user?r.beforeUser:r.beforeImage;
        if(matches(expected.value(target),before))continue;
        if(fail(c,user?FailurePoint::RollbackConfig:FailurePoint::RollbackImage)){error=QStringLiteral("Injected partial rollback failure; the journal is retained.");return false;}
        if(fingerprint(files(c))!=fingerprint(expected)){error=QStringLiteral("A foreign edit appeared during rollback; it was preserved.");return false;}
        File stagedIdentity;if(!stageFile(c,user?StageUser:StageImage,before,error,&stagedIdentity))return false;
        const auto recordNow=load(c,Journal,JournalLimit);
        if(!recordNow.ok||recordNow.bytes!=journalBytes(r)||fingerprint(files(c))!=fingerprint(expected)){error=QStringLiteral("The journal or files changed at the rollback boundary; foreign edits were preserved.");return false;}
        const auto boundaryCurrent=[&]{const auto record=load(c,Journal,JournalLimit);return record.ok&&record.bytes==journalBytes(r)&&trusted(c,Journal,false)&&fingerprint(files(c))==fingerprint(expected);};
        if(!install(c,user?StageUser:StageImage,target,before,stagedIdentity,boundaryCurrent,error))return false;
        expected[target]=load(c,target,limit(target));
    }
    const auto after=files(c);if(fingerprint(after)!=fingerprint(expected)||!matches(after.value(User),r.beforeUser)||!matches(after.value(Background),r.beforeImage)){error=QStringLiteral("Rollback readback failed; the journal is retained.");return false;}
    return cleanup(c,r,error);
}
}
QString themeDirectory(const Context &c){return path(c,Theme);}
QStringList watchedPaths(const Context &c){QStringList out;for(const auto &name:dataNames())out<<path(c,name);out<<path(c,Journal)<<path(c,Lock);return out;}
AeroLoginSnapshot read(const Context &c,QString *error){return readImpl(c,false,error);}
Result apply(const Context &c,const AeroLoginSnapshot &expected,const AeroLoginSnapshot &requested)
{
    Result out;QString error;
    if(!contextValid(c)||(!c.fixture&&::geteuid()!=0)){out.error=QStringLiteral("Administrative authorization and a valid fixed filesystem context are required.");return out;}
    auto fresh=read(c,&error);
    if(!fresh.sourceValid||!fresh.trustedInstallation||!fresh.writable||fresh.recoveryPending||fresh.revision!=expected.revision){out.error=error.isEmpty()?QStringLiteral("The approved files changed or installation is not trusted; no mutation was attempted."):error;out.snapshot=fresh;return out;}
    auto baseline=expected,request=requested;baseline.helperAvailable=fresh.helperAvailable;baseline.helperReason=fresh.helperReason;request.helperAvailable=fresh.helperAvailable;request.helperReason=fresh.helperReason;
    if(!AeroLoginTypes::sameConfiguration(baseline,fresh)||!AeroLoginTypes::requestValid(baseline,request,error)||!AeroLoginTypes::dirty(request)){out.error=error.isEmpty()?QStringLiteral("No valid explicit Aero login change was requested."):error;return out;}
    const auto lockFile=load(c,Lock,ConfigLimit);const auto beforeLockFiles=files(c);
    if(lockFile.present)for(const auto &f:beforeLockFiles)if(f.present&&f.st.st_dev==lockFile.st.st_dev&&f.st.st_ino==lockFile.st.st_ino){out.error=QStringLiteral("The transaction lock aliases a fixed source.");return out;}
    HeldLock lock;if(!lock.acquire(c,error)){out.error=error;return out;}
    fresh=read(c,&error);if(fresh.revision!=expected.revision||!fresh.writable){out.error=QStringLiteral("The fixed sources changed while acquiring the transaction lock.");return out;}
    for(const auto &name:{StageUser,StageImage})if(load(c,name,AeroLoginTypes::MaximumImageBytes).present){out.error=QStringLiteral("A staging file already exists; no file was overwritten.");return out;}
    const auto original=files(c);Recovery recovery;recovery.staticRevision=fingerprint(original,true);recovery.beforeUser=state(original.value(User),c);recovery.afterUser=recovery.beforeUser;recovery.beforeImage=state(original.value(Background),c);recovery.afterImage=recovery.beforeImage;
    if(!request.intents.isEmpty()){
        recovery.afterUser.bytes=patch(recovery.beforeUser.bytes,request);recovery.afterUser.present=recovery.beforeUser.present||!recovery.afterUser.bytes.isEmpty();recovery.afterUser.digest=recovery.afterUser.present?hash(recovery.afterUser.bytes):QByteArray();
        if(recovery.afterUser.bytes.size()>ConfigLimit||!unownedPreserved(recovery.beforeUser.bytes,recovery.afterUser.bytes,request)){out.error=QStringLiteral("The typed key patch would alter unowned INI semantics or exceed its bound.");return out;}
    }
    if(request.backgroundIntent!=AeroLoginBackgroundIntent::Keep){
        auto &image=recovery.afterImage;
        if(request.backgroundIntent==AeroLoginBackgroundIntent::RemoveOverride){image.present=false;image.bytes.clear();image.digest.clear();}
        else {
            image.present=true;image.bytes=request.backgroundIntent==AeroLoginBackgroundIntent::ReplaceImage?request.replacement.bytes:original.value(Default).bytes;image.digest=hash(image.bytes);
            if(image.bytes.isEmpty()||image.bytes.size()>AeroLoginTypes::MaximumImageBytes||!c.validateImage||!c.validateImage(image.bytes,error)){out.error=error.isEmpty()?QStringLiteral("Independent image validation is required before writing an image."):error;return out;}
        }
    }
    // Identity and safety must still match after a potentially asynchronous validator.
    if(read(c).revision!=expected.revision){out.error=QStringLiteral("The source files changed during validation.");return out;}
    State journalState;journalState.present=true;journalState.bytes=journalBytes(recovery);journalState.digest=hash(journalState.bytes);journalState.mode=0644;journalState.uid=c.expectedUid;journalState.gid=quint32(::getegid());
    if(!stageFile(c,Journal,journalState,error)||fail(c,FailurePoint::JournalSync)||!syncDirectory(c)){out.error=error.isEmpty()?QStringLiteral("Could not make the recovery journal durable."):error;out.snapshot=read(c);out.recoveryRequired=out.snapshot.recoveryPending;return out;}
    auto failed=[&](const QString &message){Result result;QString rollbackError;result.rolledBack=rollback(c,recovery,rollbackError);result.recoveryRequired=!result.rolledBack;result.saved=!result.rolledBack;result.error=message+(rollbackError.isEmpty()?QString():QLatin1Char('\n')+rollbackError);result.snapshot=read(c);return result;};
    const bool userChange=!matches(original.value(User),recovery.afterUser),imageChange=!matches(original.value(Background),recovery.afterImage);
    File stagedUser,stagedImage;
    if((userChange&&(fail(c,FailurePoint::StageConfig)||!stageFile(c,StageUser,recovery.afterUser,error,&stagedUser)))||(imageChange&&(fail(c,FailurePoint::StageImage)||!stageFile(c,StageImage,recovery.afterImage,error,&stagedImage))))return failed(error.isEmpty()?QStringLiteral("Injected transaction staging failure."):error);
    int commits=0;auto expectedFiles=original;
    for(const auto &target:{User,Background}){
        const bool user=target==User,changed=user?userChange:imageChange;if(!changed)continue;
        if(fail(c,user?FailurePoint::CommitConfig:FailurePoint::CommitImage))return failed(QStringLiteral("Injected transaction commit failure."));
        const auto current=files(c);
        if(fingerprint(current)!=fingerprint(expectedFiles))return failed(QStringLiteral("A source changed immediately before commit; foreign edits were preserved."));
        const auto boundaryCurrent=[&]{const auto record=load(c,Journal,JournalLimit);return record.ok&&record.bytes==journalBytes(recovery)&&trusted(c,Journal,false)&&fingerprint(files(c))==fingerprint(expectedFiles);};
        if(!install(c,user?StageUser:StageImage,target,user?recovery.afterUser:recovery.afterImage,user?stagedUser:stagedImage,boundaryCurrent,error))return failed(error);
        expectedFiles[target]=load(c,target,limit(target));
        if(!matches(expectedFiles.value(target),user?recovery.afterUser:recovery.afterImage))return failed(QStringLiteral("The committed file identity or content could not be read back."));
        ++commits;if(fail(c,commits==1?FailurePoint::CrashAfterFirstCommit:FailurePoint::CrashAfterSecondCommit)){out.saved=true;out.recoveryRequired=true;out.error=QStringLiteral("Simulated crash: a durable recovery journal remains. No automatic rollback was attempted.");out.snapshot=read(c);return out;}
    }
    const bool readbackFailed=fail(c,FailurePoint::Readback);const auto final=files(c);auto actual=readImpl(c,true,nullptr);QString verification;
    if(readbackFailed||!matches(final.value(User),recovery.afterUser)||!matches(final.value(Background),recovery.afterImage)||!AeroLoginTypes::matchesRequest(baseline,request,actual,verification))return failed(verification.isEmpty()?QStringLiteral("Post-commit verification failed."):verification);
    if(!cleanup(c,recovery,error)){out.saved=true;out.recoveryRequired=true;out.error=error;out.snapshot=read(c);return out;}
    out.success=out.saved=true;out.snapshot=read(c);return out;
}
Result recover(const Context &c,const QByteArray &expectedRevision)
{
    Result out;QString error;if(!contextValid(c)||(!c.fixture&&::geteuid()!=0)){out.error=QStringLiteral("Administrative authorization and a valid fixed filesystem context are required.");return out;}const auto before=read(c,&error);
    if(before.revision!=expectedRevision||!before.canRecover){out.error=error.isEmpty()?QStringLiteral("The recovery review is stale or foreign changes block recovery."):error;out.snapshot=before;return out;}
    HeldLock lock;if(!lock.acquire(c,error)){out.error=error;return out;}
    const auto fresh=read(c);if(fresh.revision!=expectedRevision||!fresh.canRecover){out.error=QStringLiteral("Recovery sources changed while acquiring the lock.");return out;}
    const auto original=files(c);const auto recovery=journal(load(c,Journal,JournalLimit),original,c);
    if(!recovery.valid||fail(c,FailurePoint::Recovery)||!rollback(c,recovery,error)){out.error=error.isEmpty()?QStringLiteral("Recovery failed; the journal remains for explicit inspection."):error;out.recoveryRequired=true;out.snapshot=read(c);return out;}
    out.success=out.saved=out.rolledBack=true;out.snapshot=read(c);return out;
}
}
