#pragma once
#include <QObject>
#include <QDateTime>
#include <QPointer>
#include <QTimer>
#include <functional>

struct NightTimeLocationFix {
    double latitude = 0, longitude = 0;
    QDateTime timestamp;
    double horizontalAccuracy = -1;
};
Q_DECLARE_METATYPE(NightTimeLocationFix)

// Injectable acquisition boundary. Factory construction happens only on an explicit request.
class NightTimeLocationSource : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void request(int timeoutMilliseconds) = 0;
    virtual void stop() = 0;
signals:
    void position(const NightTimeLocationFix &fix);
    void failed(const QString &reason);
};
class NightTimeLocation : public QObject {
    Q_OBJECT
public:
    using Factory = std::function<NightTimeLocationSource *(QObject *)>;
    explicit NightTimeLocation(QObject *parent = nullptr);
    explicit NightTimeLocation(Factory factory, QObject *parent = nullptr);
    ~NightTimeLocation() override;
    bool isBusy() const { return m_busy; }
    // Supersedes the previous request. Tokens are caller-owned and returned unchanged.
    void requestLocation(quint64 token, int timeoutMilliseconds = 30000);
    void cancel();
signals:
    void busyChanged(bool busy);
    void finished(quint64 token, bool success, const NightTimeLocationFix &fix, const QString &error);
private:
    void dispose();
    void complete(quint64 generation, bool success, const NightTimeLocationFix &fix, const QString &error);
    Factory m_factory;
    QPointer<NightTimeLocationSource> m_source;
    QTimer m_timeout;
    quint64 m_generation = 0, m_token = 0;
    bool m_busy = false;
    QDateTime m_requestedAfter;
};
