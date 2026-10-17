#include "SettingsFileReservations.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QThread>
#include <cerrno>
#include <sys/stat.h>

namespace SettingsFileReservations {
namespace {
struct Entry { Reservation reservation; QStringList keys; };
QMap<quint64,Entry> &registry(){static QMap<quint64,Entry> value;return value;}
quint64 &generation(){static quint64 value=0;return value;}
quint64 &nextId(){static quint64 value=0;return value;}
void gui(){Q_ASSERT(!QCoreApplication::instance()||QThread::currentThread()==QCoreApplication::instance()->thread());}
QString canonicalPath(const QString &path)
{
    const auto canonical=QFileInfo(path).canonicalFilePath();if(!canonical.isEmpty())return QDir::cleanPath(canonical);
    QString parent=QFileInfo(path).absolutePath();QStringList tail{QFileInfo(path).fileName()};
    while(!QFileInfo(parent).exists()&&!QFileInfo(parent).isSymLink()&&parent!=QLatin1String("/")){
        tail.prepend(QFileInfo(parent).fileName());parent=QFileInfo(parent).absolutePath();
    }
    const QFileInfo ancestor(parent);const auto base=ancestor.canonicalFilePath();
    if(base.isEmpty()||!ancestor.isDir())return {};
    return QDir::cleanPath(base+QLatin1Char('/')+tail.join(QLatin1Char('/')));
}
}
Snapshot snapshot()
{
    gui();Snapshot out;out.generation=generation();
    for(const auto &entry:registry())out.active.append({entry.reservation.paths,entry.keys,entry.reservation.epoch});
    return out;
}
QStringList currentKeys(const QStringList &paths,QString *error)
{
    if(error)error->clear();QStringList keys;
    const auto failed=[&](const QString &message){if(error)*error=message;return QStringList{};};
    if(paths.isEmpty())return failed(QStringLiteral("No settings-file paths were supplied for reservation."));
    for(const auto &path:paths){
        if(!QFileInfo(path).isAbsolute())return failed(QStringLiteral("Settings-file reservations require absolute paths."));
        const auto canonical=canonicalPath(path);if(canonical.isEmpty())return failed(QStringLiteral("A settings-file path or missing-path ancestor is ambiguous."));
        struct stat before{},after{};const auto encoded=QFile::encodeName(path);
        if(::stat(encoded.constData(),&before)==0){
            if(!S_ISREG(before.st_mode))return failed(QStringLiteral("A settings-file reservation target is not a regular file."));
            if(::stat(encoded.constData(),&after)!=0||after.st_dev!=before.st_dev||after.st_ino!=before.st_ino||canonicalPath(path)!=canonical)
                return failed(QStringLiteral("A settings-file identity changed during reservation inspection."));
            keys<<QStringLiteral("inode:%1:%2").arg(quint64(before.st_dev)).arg(quint64(before.st_ino));
        }else{
            if(errno!=ENOENT||QFileInfo(path).isSymLink())return failed(QStringLiteral("A settings-file identity cannot be proved while reserving it."));
            if(QFileInfo(path).exists()||canonicalPath(path)!=canonical)return failed(QStringLiteral("A missing settings-file path changed during reservation inspection."));
        }
        keys<<QStringLiteral("path:")+canonical;
    }
    keys.removeDuplicates();keys.sort();return keys;
}
Inspection inspect(const QStringList &paths,const Snapshot &captured)
{
    Inspection out;out.generation=captured.generation;out.paths=paths;out.paths.removeDuplicates();
    out.keys=currentKeys(out.paths,&out.error);if(out.keys.isEmpty())return out;
    bool allExisting=true;
    for(const auto &path:out.paths){struct stat metadata{};if(::stat(QFile::encodeName(path).constData(),&metadata)!=0||!S_ISREG(metadata.st_mode))allExisting=false;}
    for(const auto &active:captured.active){
        if(!active.epoch){out.error=QStringLiteral("A settings-file reservation has no mutation epoch.");return out;}
        const auto before=active.epoch->load();
        QString error;const auto keys=currentKeys(active.paths,&error);
        if(keys.isEmpty()){out.error=error;return out;}
        if(active.epoch->load()!=before){out.error=QStringLiteral("A tracked settings-file identity changed during admission.");return out;}
        const auto held=active.retainedKeys+keys;
        for(const auto &key:out.keys)if(held.contains(key)){out.error=QStringLiteral("Another tracked settings operation owns this file or an alias. Wait for terminal completion.");return out;}
        // A native atomic replacement cannot adopt an unrelated existing inode.
        // Missing candidates remain ambiguous while a mutation is in flight.
        // This is application coordination, not a hostile-filesystem CAS lock.
        if(before%2&&!allExisting){out.error=QStringLiteral("A tracked settings-file mutation is in flight; separation of missing candidate files cannot yet be proved. Wait for its completion.");return out;}
        out.activeKeys<<held;out.observedEpochs.append(qMakePair(active.epoch,before));
    }
    QString finalError;
    if(currentKeys(out.paths,&finalError)!=out.keys){out.error=finalError.isEmpty()?QStringLiteral("Candidate settings-file identities changed during admission."):finalError;return out;}
    out.success=true;return out;
}
Reservation acquire(const Inspection &inspection,QString &error)
{
    gui();error.clear();
    if(!inspection.success||inspection.keys.isEmpty()||inspection.paths.isEmpty()){error=inspection.error.isEmpty()?QStringLiteral("Settings-file inspection did not prove identity separation."):inspection.error;return {};}
    if(inspection.generation!=generation()){error=QStringLiteral("Another settings operation changed while this save was being prepared. Revert to reload the current preferences before trying again.");return {};}
    for(const auto &observed:inspection.observedEpochs)if(!observed.first||observed.first->load()!=observed.second){error=QStringLiteral("A tracked settings-file mutation changed during admission.");return {};}
    QStringList active=inspection.activeKeys;for(const auto &entry:registry())active<<entry.keys;
    for(const auto &key:inspection.keys)if(active.contains(key)){error=QStringLiteral("Another tracked settings operation owns this file or an alias. Wait for terminal completion.");return {};}
    Reservation out;out.id=++nextId();out.paths=inspection.paths;out.epoch=std::make_shared<std::atomic<quint64>>(0);
    registry().insert(out.id,{out,inspection.keys});++generation();return out;
}
bool refresh(const Reservation &reservation,const QStringList &keys)
{
    gui();auto it=registry().find(reservation.id);
    if(!reservation.valid()||it==registry().end()||it->reservation.epoch!=reservation.epoch||keys.isEmpty())return false;
    it->keys<<keys;it->keys.removeDuplicates();++generation();return true;
}
void release(const Reservation &reservation)
{
    gui();const auto it=registry().find(reservation.id);
    if(it!=registry().end()&&it->reservation.epoch==reservation.epoch){registry().erase(it);++generation();}
}
bool beginMutation(const Reservation &reservation)
{
    if(!reservation.valid())return false;auto value=reservation.epoch->load();
    while(!(value%2)){if(reservation.epoch->compare_exchange_weak(value,value+1))return true;}return false;
}
void endMutation(const Reservation &reservation)
{
    if(!reservation.valid())return;auto value=reservation.epoch->load();
    while(value%2){if(reservation.epoch->compare_exchange_weak(value,value+1))return;}
}
}
