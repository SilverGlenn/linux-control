#pragma once

#include <QJsonObject>
#include <QList>
#include <QPoint>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

struct DisplayMode {
    QString id;
    int width = 0, height = 0;
    double refresh = 0;
    QString label() const;
};
struct DisplayOutput {
    int id = 0;
    QString name, currentMode;
    bool connected = false, enabled = false;
    QList<DisplayMode> modes;
    QPoint position;
    double scale = 1;
    // Exact KScreen::Output::Rotation flags: 1,2,4,8,16,32,64,128.
    int rotation = 1, priority = 0, replicationSource = 0;
    bool followPreferredMode = false;
};
struct DisplaySnapshot {
    QJsonObject json;
    QList<DisplayOutput> outputs;
    const DisplayOutput *output(int id) const;
    DisplayOutput *output(int id);
    bool isValid() const { return !json.isEmpty(); }
};
namespace DisplayProtocol {
// Audited KWin/Wayland bounds. The compositor rounds signed 24.8 fixed-point
// scale values to 1/120; its largest read value cannot itself be sent back.
inline constexpr double MinimumScale = 1.0 / 120.0;
inline constexpr double MaximumWrittenScale = 8388607.9916666667;
inline constexpr double MaximumReadScale = 8388608.0;
inline constexpr int MaximumPosition = 1000000;
// KScreen 6.6.5 custom field bounds; existing presets include 400%.
inline constexpr double CustomMinimumScale = 0.5;
inline constexpr double CustomMaximumScale = 3.0;
inline constexpr double MaximumEditorScale = 4.0;

// Exact installed libkscreen 6.6.4 ConfigSerializer schema. No human/ANSI parsing.
// Production readers attach a pinned KWin/package identity in controlIdentity.
// Isolated fixtures may omit that identity; production watchdog reads never do.
bool parse(const QByteArray &json, DisplaySnapshot &snapshot, QString &error);
// Quantizes only a scale explicitly changed relative to its original JSON.
// Untouched saved values retain their exact value, including outside UI bounds.
DisplaySnapshot normalized(const DisplaySnapshot &snapshot);
bool same(const DisplaySnapshot &a, const DisplaySnapshot &b);
bool canEdit(const DisplaySnapshot &, int outputId, QString &error);
bool canScale(const DisplaySnapshot &);
bool canRotate(const DisplaySnapshot &);
bool canPosition(const DisplaySnapshot &);
bool canChange(const DisplaySnapshot &baseline, const DisplaySnapshot &requested, QString &error);
bool matchesRequested(const DisplaySnapshot &baseline, const DisplaySnapshot &requested, const DisplaySnapshot &actual);
QStringList changeArguments(const DisplaySnapshot &baseline, const DisplaySnapshot &requested);
// Each field must equal an endpoint value; positions may additionally share
// one proven native origin shift across the complete partially applied layout.
// Untouched layout and all identity/catalog fields must still match.
bool ownedIntermediate(const DisplaySnapshot &baseline, const DisplaySnapshot &requested, const DisplaySnapshot &actual);
bool rollbackRequest(const DisplaySnapshot &baseline, const DisplaySnapshot &requested, const DisplaySnapshot &actual,
                     DisplaySnapshot &restoration, QString &error);
QProcessEnvironment doctorEnvironment();
// Compatibility wrappers for the earlier mode-only editor/fixtures.
bool matchesMode(const DisplaySnapshot &baseline, const DisplaySnapshot &actual, int outputId, const QString &mode);
bool canChange(const DisplaySnapshot &snapshot, int outputId, const QString &mode, QString &error);
QString modeArgument(int outputId, const QString &mode);
}
