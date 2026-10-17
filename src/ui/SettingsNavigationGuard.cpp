#include "SettingsNavigationGuard.h"
#include "settings/SettingsSession.h"

void SettingsNavigationGuard::request(SettingsSession *session, Prompt prompt, Action action)
{
    if (m_pending) return;
    m_pending = true;
    m_session = session;
    m_prompt = std::move(prompt);
    m_action = std::move(action);
    emit pendingChanged();
    if (session) {
        m_destroyConnection = connect(session, &QObject::destroyed, this, [this]() { clear(); });
    }
    decide();
}

void SettingsNavigationGuard::decide()
{
    if (!m_session) { proceed(); return; }
    if (m_session->isBusy()) {
        m_stateConnection = connect(m_session, &SettingsSession::stateChanged, this, [this]() {
            if (m_session && !m_session->isBusy()) {
                disconnect(m_stateConnection);
                decide();
            }
        });
        return;
    }
    if (m_discarding) {
        if (!m_session->isDirty() && !m_session->hasConflict() && m_session->errorText().isEmpty()) proceed();
        else clear(); // A failed asynchronous Revert keeps its draft and error visible.
        return;
    }
    if (!m_session->isDirty()) { proceed(); return; }
    const Choice choice = m_choice ? *m_choice : m_prompt();
    if (!m_session) { clear(); return; }
    if (choice == Choice::Cancel) { clear(); return; }
    // A modal prompt runs a nested event loop. An external settings refresh
    // can start there, so wait for it before applying or discarding.
    if (m_session->isBusy()) {
        m_choice = choice;
        decide();
        return;
    }
    if (choice == Choice::Discard) {
        m_choice.reset();
        m_discarding = true;
        m_session->revert();
        decide();
        return;
    }
    m_applyConnection = connect(m_session, &SettingsSession::applyFinished, this,
        [this](bool success) {
            if (success && m_session && !m_session->isDirty()) proceed();
            else clear(); // Stay on the page with the backend's error visible.
        });
    m_session->apply();
}

void SettingsNavigationGuard::clear()
{
    disconnect(m_stateConnection);
    disconnect(m_applyConnection);
    disconnect(m_destroyConnection);
    m_action = {};
    m_prompt = {};
    m_choice.reset();
    m_discarding = false;
    m_session = nullptr;
    m_pending = false;
    emit pendingChanged();
}

void SettingsNavigationGuard::proceed()
{
    auto action = std::move(m_action);
    clear();
    if (action) action();
}
