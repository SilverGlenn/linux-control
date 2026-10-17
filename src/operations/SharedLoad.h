#pragma once

#include "OperationManager.h"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QPointer>
#include <QtConcurrent>

#include <functional>
#include <utility>
#include <vector>

// App-owned asynchronous read cache. A completed value can be shown
// immediately on the next visit while a fresh read runs in the background.
// Subscribers are guarded by QPointer, so a destroyed page is never called.
template <typename T>
class SharedLoad final : public QObject {
public:
    using Loader = std::function<T()>;
    using Subscriber = std::function<void(const T &, bool fresh)>;

    explicit SharedLoad(QString activityTitle, QObject *parent = nullptr)
        : QObject(parent), m_activityTitle(std::move(activityTitle)) {}

    bool isRunning() const { return m_running; }
    bool hasResult() const { return m_hasResult; }
    T result() const { return m_result; }

    // Cached data is delivered synchronously with fresh=false, then a new
    // worker read starts for this visit. If a read is already in flight, the
    // caller also subscribes to its completion instead of starting a duplicate.
    void subscribe(QObject *context, Loader loader, Subscriber subscriber)
    {
        QPointer<QObject> guardedContext(context);
        if (!guardedContext || !loader || !subscriber)
            return;

        SubscriberEntry entry{guardedContext, std::move(subscriber)};
        if (m_hasResult) {
            entry.callback(m_result, false);
            if (!entry.context)
                return;
        }

        pruneSubscribers();
        if (m_running) {
            m_subscribers.push_back(std::move(entry));
            return;
        }

        m_subscribers.push_back(std::move(entry));
        start(std::move(loader));
    }

private:
    struct SubscriberEntry {
        QPointer<QObject> context;
        Subscriber callback;
    };

    void pruneSubscribers()
    {
        std::vector<SubscriberEntry> alive;
        alive.reserve(m_subscribers.size());
        for (auto &entry : m_subscribers)
            if (entry.context)
                alive.push_back(std::move(entry));
        m_subscribers = std::move(alive);
    }

    void start(Loader loader)
    {
        m_running = true;
        OperationManager::instance()->setActivity(this, m_activityTitle, true);

        auto *watcher = new QFutureWatcher<T>(this);
        m_watcher = watcher;
        connect(watcher, &QFutureWatcher<T>::finished, this,
                [this, watcher]() {
            const T value = watcher->result();
            watcher->deleteLater();
            if (m_watcher == watcher)
                m_watcher = nullptr;

            m_result = value;
            m_hasResult = true;
            m_running = false;
            OperationManager::instance()->setActivity(
                this, m_activityTitle, false);

            // A callback may destroy another subscriber, so check each weak
            // context immediately before dispatching it.
            const auto subscribers = std::move(m_subscribers);
            m_subscribers.clear();
            for (const SubscriberEntry &entry : subscribers)
                if (entry.context)
                    entry.callback(m_result, true);
        });
        watcher->setFuture(QtConcurrent::run(std::move(loader)));
    }

    QString m_activityTitle;
    QFutureWatcher<T> *m_watcher = nullptr;
    T m_result{};
    bool m_hasResult = false;
    bool m_running = false;
    std::vector<SubscriberEntry> m_subscribers;
};
