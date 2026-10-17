#include "ShortcutsTypes.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QSet>
#include <algorithm>
#include <tuple>

bool ShortcutActionId::operator<(const ShortcutActionId &other) const
{ return std::tie(component,context,action) < std::tie(other.component,other.context,other.action); }
QStringList ShortcutActionId::nativeId(const QString &componentFriendly, const QString &actionFriendly) const
{ return {component + QLatin1Char('|') + context, action, componentFriendly, actionFriendly}; }
const ShortcutAction *ShortcutsSnapshot::find(const ShortcutActionId &id) const
{ for (const auto &item : actions) if (item.id == id) return &item; return nullptr; }
namespace {
QKeySequence normalized(const QKeySequence &sequence)
{
    int keys[4] = {};
    for (int i=0;i<sequence.count();++i) {
        const int combined=sequence[i].toCombined();
        const int key=combined & ~Qt::KeyboardModifierMask, mods=combined & Qt::KeyboardModifierMask;
        if ((mods & Qt::ShiftModifier) && (key==Qt::Key_Tab || key==Qt::Key_Backtab)) keys[i]=mods|Qt::Key_Tab;
        else if (key==Qt::Key_Shift) keys[i]=mods|Qt::ShiftModifier;
        else if (key==Qt::Key_Control) keys[i]=mods|Qt::ControlModifier;
        else if (key==Qt::Key_Alt) keys[i]=mods|Qt::AltModifier;
        else if (key==Qt::Key_Meta) keys[i]=mods|Qt::MetaModifier;
        else keys[i]=combined;
    }
    return QKeySequence(keys[0],keys[1],keys[2],keys[3]);
}
QSet<QKeySequence> keySet(const QList<QKeySequence> &keys)
{ QSet<QKeySequence> result; for (const auto &key : keys) if (!key.isEmpty()) result.insert(key); return result; }
bool contains(const QKeySequence &large, const QKeySequence &small)
{
    if (small.isEmpty() || small.count()>large.count()) return false;
    for (int offset=0;offset<=large.count()-small.count();++offset) {
        bool equal=true;
        for (int i=0;i<small.count();++i) if (small[i]!=large[offset+i]) { equal=false; break; }
        if (equal) return true;
    }
    return false;
}
}
namespace ShortcutsTypes {
bool sameKeys(const QList<QKeySequence> &a, const QList<QKeySequence> &b)
{ return keySet(a)==keySet(b); }
bool conflicts(const QKeySequence &a, const QKeySequence &b)
{ const auto na=normalized(a), nb=normalized(b); return contains(na,nb)||contains(nb,na); }
bool validate(const ShortcutsSnapshot &snapshot, const ShortcutChanges &requested, QString &error)
{
    if (!snapshot.available || !snapshot.writable) { error=snapshot.reason.isEmpty()?QObject::tr("Shortcut bindings are read-only."):snapshot.reason; return false; }
    for (auto it=requested.cbegin();it!=requested.cend();++it) {
        const auto *action=snapshot.find(it.key());
        if (!action) { error=QObject::tr("The requested shortcut action is unavailable. Refresh before applying."); return false; }
        if (sameKeys(action->current,it.value())) continue;
        if (!action->available) { error=QObject::tr("The requested shortcut action is read-only."); return false; }
        if (!action->registered && (!action->layoutAction || !snapshot.layouts.available
            || !snapshot.layouts.actionNames.contains(action->id.action))) {
            error=QObject::tr("This shortcut action cannot be registered by the installed provider."); return false;
        }
        if (it.value().size()>64) { error=QObject::tr("An action supports at most 64 alternatives in this editor."); return false; }
        QSet<QKeySequence> seen;
        for (const auto &key : it.value()) {
            if (key.isEmpty()) { error=QObject::tr("Remove an empty alternative to disable a shortcut."); return false; }
            for (int i=0;i<key.count();++i) if (key[i].key()==Qt::Key_unknown) { error=QObject::tr("The shortcut contains an unknown key."); return false; }
            const auto n=normalized(key);
            if (seen.contains(n)) { error=QObject::tr("The same shortcut alternative appears more than once."); return false; }
            for (const auto &other : seen) if (conflicts(n,other)) { error=QObject::tr("Shortcut alternatives shadow each other."); return false; }
            seen.insert(n);
        }
        // Unchanged alternatives, including pre-existing overlaps, must survive unrelated edits.
        for (const auto &other : snapshot.actions) {
            if (other.id==it.key()) continue;
            if (other.id.component==it.key().component && other.id.context!=it.key().context) continue;
            const auto final=requested.contains(other.id)?requested.value(other.id):other.current;
            for (const auto &key : it.value()) for (const auto &occupied : final)
                if (!action->current.contains(key) && conflicts(key,occupied)) {
                    error=QObject::tr("%1 conflicts with %2 (%3). Reassign it explicitly before applying.")
                        .arg(key.toString(QKeySequence::NativeText),other.friendlyName,other.id.component);
                    return false;
                }
        }
    }
    return true;
}
QByteArray revision(const ShortcutsSnapshot &snapshot)
{
    QByteArray bytes; QDataStream stream(&bytes,QIODevice::WriteOnly);
    stream<<snapshot.owner<<snapshot.interfaceSignature<<snapshot.configRevision;
    stream<<snapshot.layouts.owner<<snapshot.layouts.version<<snapshot.layouts.reason
          <<snapshot.layouts.revision<<snapshot.layouts.configRevision
          <<snapshot.layouts.actionNames<<snapshot.layouts.available;
    auto actions=snapshot.actions;
    std::sort(actions.begin(),actions.end(),[](const auto &a,const auto &b){return a.id<b.id;});
    for (const auto &a : actions) stream<<a.id.component<<a.id.context<<a.id.action<<a.componentFriendly<<a.contextFriendly<<a.friendlyName<<a.current<<a.defaults<<a.available<<a.registered<<a.layoutAction;
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
}
}
