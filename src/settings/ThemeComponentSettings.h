#pragma once

#include "SettingsSession.h"
#include <QStringList>
#include <QList>
#include <array>

enum class ThemeComponent { Icons = 0, Cursors = 1, ApplicationStyle = 2 };
constexpr int themeComponentCount = 3;
struct ThemeComponentChoice { QString id; QString name; };
using ThemeComponentChoices = std::array<QList<ThemeComponentChoice>, themeComponentCount>;
struct ThemeComponentField {
    QString value;
    QList<ThemeComponentChoice> choices;
    bool writable = false;
    bool reloadAvailable = false;
    QString unavailableReason;
    bool canApply() const { return writable && reloadAvailable; }
};
struct ThemeComponentState {
    std::array<ThemeComponentField, themeComponentCount> fields;
    QStringList values() const;
};
Q_DECLARE_METATYPE(ThemeComponentState)

class ThemeComponentBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent() = 0;
    // Recheck the entire expected baseline and all changed fields' policy
    // immediately before writing. Completion is followed by session readback.
    virtual void writeComponents(const QStringList &expected, const QStringList &requested) = 0;
signals:
    void readFinished(bool success, const ThemeComponentState &state, const QString &error);
    void writeFinished(bool success, const QString &error);
    void externalChange();
};

class ThemeComponentSettings final : public SettingsSession {
    Q_OBJECT
public:
    static ThemeComponentSettings *instance();
    explicit ThemeComponentSettings(ThemeComponentBackend *backend, QObject *parent = nullptr);
    bool isReady() const { return m_ready; }
    bool canApply() const;
    const ThemeComponentField &field(ThemeComponent component) const;
    QString appliedValue(ThemeComponent component) const;
    QString stagedValue(ThemeComponent component) const;
    static bool containsChoice(const ThemeComponentField &field, const QString &id);
    static bool isApprovedChoice(ThemeComponent component, const QString &id);
public slots:
    void selectComponent(ThemeComponent component, const QString &id);
    void apply() override;
    void revert() override;
    void refresh();
private:
    enum class Phase { Idle, Refresh, Preflight, Write, Readback };
    void read();
    void onRead(bool success, const ThemeComponentState &state, const QString &error);
    void onWrite(bool success, const QString &error);
    void finish(bool completesApply = false, bool success = false);
    ThemeComponentBackend *m_backend;
    ThemeComponentState m_state;
    QStringList m_staged = {QString(), QString(), QString()};
    QStringList m_requested;
    QString m_writeError;
    Phase m_phase = Phase::Idle;
    bool m_ready = false;
    bool m_pendingRefresh = false;
    bool m_writeSuccess = false;
    bool m_keepError = false;
};
