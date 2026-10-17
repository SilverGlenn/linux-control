#pragma once

#include <QObject>
#include <QString>

// A session outlives its views. Selection changes are staged; only apply()
// writes settings. applyFinished is emitted after the backend is read back.
class SettingsSession : public QObject {
    Q_OBJECT
public:
    explicit SettingsSession(QObject *parent = nullptr) : QObject(parent) {}
    bool isDirty() const { return m_dirty; }
    bool isBusy() const { return m_busy; }
    bool hasConflict() const { return m_conflict; }
    QString errorText() const { return m_error; }
    QString error() const { return m_error; }
    QString resultText() const { return m_result; }

public slots:
    virtual void apply() = 0;
    virtual void revert() = 0;

signals:
    void stateChanged();
    void applyFinished(bool success);

protected:
    bool m_dirty = false;
    bool m_busy = false;
    bool m_conflict = false;
    QString m_error;
    QString m_result;
};
