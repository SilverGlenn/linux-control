#pragma once

#include "SettingsSession.h"
#include "ShortcutsTypes.h"

struct ShortcutConflict {
    ShortcutActionId id;
    QString name;
    QKeySequence sequence;
};

// The application owns this session; pages only observe and stage its draft.
class ShortcutsSettings final : public SettingsSession {
    Q_OBJECT
public:
    static ShortcutsSettings *instance();
    static ShortcutsSettings *standardInstance();
    explicit ShortcutsSettings(ShortcutsBackend *backend, QObject *parent = nullptr);
    const ShortcutsSnapshot &applied() const { return m_applied; }
    const ShortcutsSnapshot &staged() const { return m_staged; }
    bool isReady() const { return m_ready; }
    bool canApply() const;
    QList<ShortcutConflict> conflictsFor(const ShortcutActionId &id, const QList<QKeySequence> &keys) const;
    bool selectShortcuts(const ShortcutActionId &id, const QList<QKeySequence> &keys, bool reassign = false);
    bool selectChanges(const ShortcutChanges &changes, bool reassign = false);
    bool validateChanges(const ShortcutChanges &changes, bool reassign, QString &error) const;
    static bool stageTogether(ShortcutsSettings *globals, const ShortcutChanges &globalChanges,
                              ShortcutsSettings *standards, const ShortcutChanges &standardChanges,
                              bool reassign, QString &error);
public slots:
    void restoreDefaults(const ShortcutActionId &id);
    void restoreAllDefaults();
    void refresh();
    void apply() override;
    void revert() override;
private:
    enum class Phase { Idle, Reading, Writing };
    void read();
    bool stage(const ShortcutsSnapshot &candidate);
    bool prepareChanges(const ShortcutChanges &changes, bool reassign, ShortcutsSnapshot &candidate, QString &error) const;
    void commitCandidate(const ShortcutsSnapshot &candidate);
    void onRead(quint64 token, bool success, const ShortcutsSnapshot &snapshot, const QString &error);
    void onWritten(quint64 token, ShortcutsOutcome outcome, const ShortcutsSnapshot &snapshot, const QString &error);
    ShortcutsBackend *m_backend;
    ShortcutsSnapshot m_applied, m_staged, m_writeExpected;
    ShortcutChanges m_requested;
    QString m_lockedOwner;
    quint64 m_token = 0;
    Phase m_phase = Phase::Idle;
    bool m_ready = false, m_pendingRefresh = false, m_reverting = false, m_keepError = false;
};
