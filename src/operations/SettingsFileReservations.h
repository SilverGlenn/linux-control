#pragma once
#include <QStringList>
#include <QList>
#include <atomic>
#include <memory>

// GUI-owned registry; all filesystem inspection belongs in a worker. These
// values contain no QObject or reference to mutable registry storage.
namespace SettingsFileReservations {
struct Reservation {
    quint64 id=0;
    QStringList paths;
    std::shared_ptr<std::atomic<quint64>> epoch;
    bool valid() const { return id!=0&&bool(epoch); }
};
struct View {
    QStringList paths,retainedKeys;
    std::shared_ptr<std::atomic<quint64>> epoch;
};
struct Snapshot { quint64 generation=0; QList<View> active; };
struct Inspection {
    bool success=false;
    QString error;
    quint64 generation=0;
    QStringList paths,keys,activeKeys;
    QList<QPair<std::shared_ptr<std::atomic<quint64>>,quint64>> observedEpochs;
};
Snapshot snapshot(); // GUI only
Inspection inspect(const QStringList &paths,const Snapshot &snapshot); // worker only
QStringList currentKeys(const QStringList &paths,QString *error=nullptr); // worker only; empty on ambiguity
Reservation acquire(const Inspection &inspection,QString &error); // GUI only; invalid on failure
bool refresh(const Reservation &reservation,const QStringList &workerCurrentKeys); // GUI only; retains old keys
void release(const Reservation &reservation); // GUI only; invalid/already released is a no-op
bool beginMutation(const Reservation &reservation); // atomic only; false if invalid/already mutating
void endMutation(const Reservation &reservation); // atomic only; invalid/idle is a no-op
}
