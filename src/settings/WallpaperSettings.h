#pragma once

#include "SettingsSession.h"
#include <QList>

struct WallpaperDesktop {
    quint32 id = 0;
    int screen = -1;
    QString plugin;
    QString image;
    int fillMode = 2;
    int immutability = 1;
    bool writable = true;
    QString reason;
};
bool operator==(const WallpaperDesktop &a, const WallpaperDesktop &b);
struct WallpaperState {
    QList<WallpaperDesktop> desktops;
};
Q_DECLARE_METATYPE(WallpaperState)

class WallpaperBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent() = 0;
    virtual void writePicture(const WallpaperDesktop &expected, const QString &image, int fillMode) = 0;
signals:
    void readFinished(bool success, const WallpaperState &state, const QString &error);
    void writeFinished(bool success, const QString &error);
    void externalChange();
};

class WallpaperSettings final : public SettingsSession {
    Q_OBJECT
public:
    static WallpaperSettings *instance();
    explicit WallpaperSettings(WallpaperBackend *backend, QObject *parent = nullptr);
    bool isReady() const { return m_ready; }
    bool canApply() const;
    WallpaperState state() const { return m_state; }
    quint32 selectedDesktop() const { return m_selected; }
    const WallpaperDesktop *desktop() const;
    QString stagedImage() const { return m_image; }
    int stagedFillMode() const { return m_fillMode; }
    static bool supportedFillMode(int mode);
public slots:
    void selectDesktop(quint32 id);
    void selectPicture(const QString &localPath);
    void selectFillMode(int mode);
    void apply() override;
    void revert() override;
    void refresh();
private:
    enum class Phase { Idle, Refresh, Preflight, Write, Readback };
    void onRead(bool success, const WallpaperState &state, const QString &error);
    void finish(bool completesApply = false, bool success = false);
    void resetSelection();
    void updateDirty();
    WallpaperBackend *m_backend;
    WallpaperState m_state;
    WallpaperDesktop m_expected;
    quint32 m_selected = 0;
    QString m_image, m_requestedImage;
    int m_fillMode = 2, m_requestedFill = 2;
    Phase m_phase = Phase::Idle;
    bool m_ready = false, m_pendingRefresh = false, m_writeSucceeded = false;
    bool m_retainError = false;
    QString m_writeError;
};
