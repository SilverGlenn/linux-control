#pragma once

#include <QObject>
#include <QPointer>
#include <functional>
#include <optional>

class SettingsSession;

// History and page replacement happen only after the pending edit decision.
class SettingsNavigationGuard final : public QObject {
    Q_OBJECT
public:
    enum class Choice { Apply, Discard, Cancel };
    using Prompt = std::function<Choice()>;
    using Action = std::function<void()>;
    explicit SettingsNavigationGuard(QObject *parent = nullptr) : QObject(parent) {}
    bool pending() const { return m_pending; }
    void request(SettingsSession *session, Prompt prompt, Action action);
signals:
    void pendingChanged();
private:
    void decide();
    void clear();
    void proceed();
    bool m_pending = false;
    bool m_discarding = false;
    QPointer<SettingsSession> m_session;
    Prompt m_prompt;
    Action m_action;
    std::optional<Choice> m_choice;
    QMetaObject::Connection m_stateConnection, m_applyConnection, m_destroyConnection;
};
