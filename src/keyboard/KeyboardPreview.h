#pragma once

#include <QDialog>
#include <QList>
#include <QPolygonF>
#include <QStringList>

class QComboBox;
class QLabel;
class QTreeWidget;
class QWidget;

namespace KeyboardPreview {
struct Request {
    QString model, layout, variant;
    QStringList options;
};
struct Group {
    QList<QStringList> levels; // One entry per level, retaining every keysym.
    QStringList modifiers;
};
struct Key {
    QString name;
    quint32 code = 0;
    QList<Group> groups;
};
struct Shape {
    QString name;
    QPolygonF outline;
};
struct Result {
    Request request;
    QList<Key> keys;
    QStringList groups;
    QList<Shape> geometry;
    QString error, geometryReason;
    bool valid() const { return error.isEmpty() && !keys.isEmpty(); }
    const Key *key(const QString &name) const;
};
// Pure, in-process compilation of installed system XKB data. No device/server access.
Result compile(const Request &request);
}

class KeyboardPreviewDialog final : public QDialog {
    Q_OBJECT
public:
    explicit KeyboardPreviewDialog(const KeyboardPreview::Request &request, QWidget *parent = nullptr);
    const KeyboardPreview::Result &result() const { return m_result; }
signals:
    void previewReady();
private:
    void populate();
    void updateGroup();
    void updateLevel();
    KeyboardPreview::Result m_result;
    QLabel *m_status, *m_details;
    QComboBox *m_group, *m_level;
    QTreeWidget *m_keys;
    QWidget *m_diagram;
};
