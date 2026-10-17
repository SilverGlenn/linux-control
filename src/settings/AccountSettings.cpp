#include "AccountSettings.h"
#include "platform/accounts/AccountsBackend.h"
#include "operations/OperationManager.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

AccountSettings *AccountSettings::instance()
{
    static QPointer<AccountSettings> session;
    if (!session) session = new AccountSettings(new AccountsBackend, QCoreApplication::instance());
    return session;
}
AccountSettings::AccountSettings(AccountBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend);
    backend->setParent(this);
    connect(backend, &AccountBackend::readFinished, this, &AccountSettings::onRead);
    connect(backend, &AccountBackend::authorizationFinished, this, &AccountSettings::onAuthorized);
    connect(backend, &AccountBackend::writeFinished, this, &AccountSettings::onWritten);
    connect(backend, &AccountBackend::externalChange, this, &AccountSettings::refresh);
    m_busy = true;
    QTimer::singleShot(0, this, [this] { beginRead(Phase::Refresh); });
}
bool AccountSettings::validName(const QString &name)
{
    if (name.toUtf8().size() > 256 || QString::fromUtf8(name.toUtf8()) != name) return false;
    for (const QChar ch : name)
        if (ch.isNull() || ch.category() == QChar::Other_Control || ch == u':' || ch == u',' || ch == u'\n' || ch == u'\r'
            || ch.category() == QChar::Separator_Line || ch.category() == QChar::Separator_Paragraph) return false;
    return true;
}
bool AccountSettings::canCancel() const
{ return m_busy && (m_phase == Phase::Preflight || m_phase == Phase::Authorization || m_phase == Phase::FinalPreflight); }
void AccountSettings::selectName(const QString &name)
{
    if (m_busy || !m_ready) return;
    m_keepFailure = false;
    m_staged = name; m_dirty = m_staged != m_state.realName;
    m_error = validName(name) ? QString() : tr("Use at most 256 UTF-8 bytes without commas, colons, or control characters.");
    m_result.clear();
    if (!m_dirty) m_conflict = false;
    emit stateChanged();
}
void AccountSettings::apply()
{
    if (m_busy) return;
    if (!canApply() || m_conflict || !validName(m_staged)) {
        m_error = m_uncertain ? tr("An earlier request may still complete. Further changes are disabled in this session.")
            : m_conflict ? tr("Your account changed outside Control Panel. Revert to reload it before applying.")
            : !m_ready ? tr("Your current account could not be read. Revert to retry.")
            : !validName(m_staged) ? tr("The display name contains unsupported characters or is too long.")
            : m_state.unavailableReason;
        emit stateChanged(); emit applyFinished(false); return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    if (!OperationManager::instance()->acquireMutation(this)) {
        m_error = tr("Another change is in progress. Try again after it finishes.");
        emit stateChanged(); emit applyFinished(false); return;
    }
    m_reserved = true;
    m_requested = m_staged; m_error.clear(); m_result.clear(); m_keepFailure = false;
    ++m_token;
    beginRead(Phase::Preflight);
}
void AccountSettings::revert()
{
    if (m_busy) return;
    m_staged = m_state.realName; m_dirty = false; m_conflict = false; m_keepFailure = false;
    m_error.clear(); m_result.clear(); ++m_token;
    beginRead(Phase::Refresh);
}
void AccountSettings::refresh()
{
    if (m_busy) { m_pendingRefresh = true; return; }
    ++m_token; beginRead(Phase::Refresh);
}
void AccountSettings::beginRead(Phase phase)
{
    m_phase = phase; m_busy = true;
    OperationManager::instance()->setActivity(this, phase == Phase::Readback ? "Verifying current-user account name" : "Reading current-user account", true);
    emit stateChanged();
    m_backend->readCurrent(m_token);
}
void AccountSettings::onRead(quint64 token, bool success, const AccountState &state, const QString &error)
{
    if (token != m_token || (m_phase != Phase::Refresh && m_phase != Phase::Preflight && m_phase != Phase::FinalPreflight && m_phase != Phase::Readback)) return;
    const bool valid = success && !state.userName.isEmpty() && !state.objectPath.isEmpty() && !state.serviceOwner.isEmpty();
    if (m_phase == Phase::Preflight || m_phase == Phase::FinalPreflight) {
        if (!valid || !state.sameIdentity(m_state) || state.realName != m_state.realName || !state.editable) {
            if (valid) { m_state = state; m_dirty = m_staged != state.realName; }
            else m_ready = false;
            m_conflict = true;
            m_backend->cancelAuthorization(m_token);
            failApply(!valid ? tr("The current account could not be checked before changing it. %1").arg(error)
                : !state.editable ? state.unavailableReason
                : tr("The current-user identity or display name changed before Apply. Revert to reload it."));
            return;
        }
        m_state = state;
        if (m_phase == Phase::Preflight) {
            m_phase = Phase::Authorization;
            OperationManager::instance()->setActivity(this, "Authorizing current-user name change", true);
            emit stateChanged();
            m_backend->authorize(m_token);
        } else {
            m_phase = Phase::Write;
            OperationManager::instance()->setActivity(this, "Changing current-user account name", true);
            emit stateChanged();
            m_backend->writeName(m_token, state, m_requested);
        }
        return;
    }
    const bool readback = m_phase == Phase::Readback;
    const bool identityMatches = valid && state.sameIdentity(m_state);
    const bool previousDirty = m_dirty;
    const bool changed = m_ready && valid && (!state.sameIdentity(m_state) || state.realName != m_state.realName);
    if (valid) {
        const bool identityChanged = m_ready && !state.sameIdentity(m_state);
        m_state = state; m_ready = true;
        if (!previousDirty) m_staged = state.realName;
        m_dirty = m_staged != state.realName;
        if (!readback && changed && previousDirty && (m_dirty || identityChanged)) m_conflict = true;
    } else {
        m_ready = false;
        if (previousDirty || readback) m_conflict = true;
    }
    if (readback) {
        const bool applied = m_outcome == AccountOutcome::Success && valid && identityMatches && !m_uncertain
            && state.realName == m_requested;
        m_keepFailure = !applied;
        if (applied) {
            m_staged = state.realName; m_dirty = false; m_conflict = false; m_error.clear();
            m_result = tr("Your display name was saved and verified with AccountsService.");
        } else {
            if (m_outcome == AccountOutcome::Success) m_outcome = AccountOutcome::Failed;
            m_conflict = true;
            m_error = !m_writeError.isEmpty() ? m_writeError
                : !valid ? tr("The request finished, but your account could not be read back. Revert to reload it. %1").arg(error)
                : tr("AccountsService reports a different display name. The change was not verified.");
            m_result.clear();
        }
        finish(applied, true);
    } else {
        if (!valid && !m_keepFailure) m_error = error.isEmpty() ? tr("AccountsService could not read your current account.") : error;
        else if (m_conflict && m_error.isEmpty()) m_error = tr("Your account changed outside Control Panel. Revert to reload it.");
        else if (valid && !m_keepFailure && !m_conflict && validName(m_staged)) m_error.clear();
        finish();
    }
}
void AccountSettings::onAuthorized(quint64 token, AccountOutcome outcome, const QString &error)
{
    if (token != m_token || m_phase != Phase::Authorization) return;
    m_outcome = outcome;
    if (outcome != AccountOutcome::Success) {
        failApply(error.isEmpty() ? (outcome == AccountOutcome::Cancelled ? tr("Authorization was canceled. No account change was sent.")
                                 : outcome == AccountOutcome::Denied ? tr("Authorization was denied. No account change was sent.")
                                 : tr("Authorization failed. No account change was sent.")) : error, outcome);
        return;
    }
    beginRead(Phase::FinalPreflight);
}
void AccountSettings::onWritten(quint64 token, AccountOutcome outcome, const QString &error)
{
    if (token != m_token || m_phase != Phase::Write) return;
    m_outcome = outcome; m_writeError = error;
    if (outcome == AccountOutcome::Uncertain) m_uncertain = true;
    beginRead(Phase::Readback);
}
void AccountSettings::cancel()
{
    if (!canCancel()) return;
    const quint64 cancelled = m_token++;
    m_backend->cancelAuthorization(cancelled);
    m_outcome = AccountOutcome::Cancelled;
    m_keepFailure = false;
    m_error.clear(); m_result = tr("Canceled before any account change was sent. Your display name remains staged.");
    finish(false, true);
}
void AccountSettings::failApply(const QString &message, AccountOutcome outcome)
{
    m_outcome = outcome; m_error = message; m_result.clear(); m_keepFailure = true; finish(false, true);
}
void AccountSettings::finish(bool applied, bool completesApply)
{
    m_phase = Phase::Idle; m_busy = false;
    if (m_reserved) OperationManager::instance()->releaseMutation(this);
    m_reserved = false;
    OperationManager::instance()->setActivity(this, {}, false);
    emit stateChanged();
    if (completesApply) emit applyFinished(applied);
    if (m_pendingRefresh) {
        m_pendingRefresh = false;
        QTimer::singleShot(0, this, [this] { if (!m_busy) refresh(); else m_pendingRefresh = true; });
    }
}
