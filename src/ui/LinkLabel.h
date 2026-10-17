#pragma once

#include <QLabel>
#include <QFont>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QStringList>
#include <QProcess>
#include <QStandardPaths>
#include <QMessageBox>
#include <QWidget>
#include "Commands.h"

#include <QAccessibleWidget>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QPainter>

inline void installLinkLabelAccessibility();

// QLabel is retained so existing page layout and label discovery keep working.
// Every activation source uses the same enabled guard and a matched press/release.
class LinkLabel : public QLabel {
    Q_OBJECT
public:
    explicit LinkLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(text, parent)
    {
        installLinkLabelAccessibility();
        QFont f = font();
        f.setPointSize(9);
        setFont(f);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(text);
        setCursor(Qt::PointingHandCursor);
        setStyleSheet(
            "QLabel { color: #1F4E99; background: transparent; }"
            "QLabel:enabled:hover { color: #0033AA; }"
            "QLabel:disabled { color: #777777; }");
    }

    void setText(const QString &text)
    {
        if (accessibleName().isEmpty() || accessibleName() == QLabel::text())
            setAccessibleName(text);
        QLabel::setText(text);
    }

    void activate()
    {
        if (isEnabled())
            emit clicked();
    }

signals:
    void clicked();

protected:
    void changeEvent(QEvent *e) override
    {
        if (e->type() == QEvent::EnabledChange) {
            setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
            setFocusPolicy(isEnabled() ? Qt::StrongFocus : Qt::NoFocus);
            if (!isEnabled()) {
                m_mousePressed = false;
                m_pressedKey = 0;
                QFont f = font();
                f.setUnderline(false);
                setFont(f);
            }
        }
        QLabel::changeEvent(e);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (isEnabled() && e->button() == Qt::LeftButton && rect().contains(e->pos())) {
            m_mousePressed = true;
            setFocus(Qt::MouseFocusReason);
            e->accept();
        } else {
            QLabel::mousePressEvent(e);
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            const bool activateClick = m_mousePressed && rect().contains(e->pos());
            m_mousePressed = false;
            e->accept();
            if (activateClick)
                activate();
        } else {
            QLabel::mouseReleaseEvent(e);
        }
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (isEnabled() && isActivationKey(e->key()) && e->modifiers() == Qt::NoModifier) {
            if (!e->isAutoRepeat())
                m_pressedKey = e->key();
            e->accept();
        } else {
            QLabel::keyPressEvent(e);
        }
    }

    void keyReleaseEvent(QKeyEvent *e) override
    {
        if (isActivationKey(e->key())) {
            const bool activateKey = m_pressedKey == e->key() && !e->isAutoRepeat();
            if (!e->isAutoRepeat())
                m_pressedKey = 0;
            e->accept();
            if (activateKey)
                activate();
        } else {
            QLabel::keyReleaseEvent(e);
        }
    }

    void focusInEvent(QFocusEvent *e) override
    {
        QLabel::focusInEvent(e);
        update();
    }

    void focusOutEvent(QFocusEvent *e) override
    {
        m_mousePressed = false;
        m_pressedKey = 0;
        QLabel::focusOutEvent(e);
        update();
    }

    void paintEvent(QPaintEvent *e) override
    {
        QLabel::paintEvent(e);
        if (hasFocus() && isEnabled()) {
            // Paint explicitly: the Aero stylesheet suppresses native focus cues.
            QPainter p(this);
            p.setPen(QPen(QColor("#1F4E99"), 1, Qt::DotLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(rect().adjusted(1, 1, -2, -2));
        }
    }

    void enterEvent(QEnterEvent *e) override
    {
        QFont f = font();
        f.setUnderline(isEnabled());
        setFont(f);
        QLabel::enterEvent(e);
    }

    void leaveEvent(QEvent *e) override
    {
        QFont f = font();
        f.setUnderline(false);
        setFont(f);
        QLabel::leaveEvent(e);
    }

private:
    static bool isActivationKey(int key)
    {
        return key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space;
    }
    bool m_mousePressed = false;
    int m_pressedKey = 0;
};

class AccessibleLinkLabel : public QAccessibleWidget {
public:
    explicit AccessibleLinkLabel(LinkLabel *link)
        : QAccessibleWidget(link, QAccessible::Link) {}

    QString text(QAccessible::Text type) const override
    {
        if (type == QAccessible::Name && widget()->accessibleName().isEmpty())
            return static_cast<LinkLabel *>(widget())->text();
        return QAccessibleWidget::text(type);
    }

    QAccessible::State state() const override
    {
        auto result = QAccessibleWidget::state();
        result.linked = true;
        return result;
    }

    QStringList actionNames() const override
    {
        return widget()->isEnabled() ? QStringList{pressAction()} : QStringList{};
    }

    void doAction(const QString &action) override
    {
        if (action == pressAction())
            static_cast<LinkLabel *>(widget())->activate();
    }

    QStringList keyBindingsForAction(const QString &action) const override
    {
        return action == pressAction() ? QStringList{QStringLiteral("Enter"),
            QStringLiteral("Space")} : QStringList{};
    }
};

inline void installLinkLabelAccessibility()
{
    static const bool installed = [] {
        QAccessible::installFactory([](const QString &, QObject *object) -> QAccessibleInterface * {
            if (auto *link = qobject_cast<LinkLabel *>(object))
                return new AccessibleLinkLabel(link);
            return nullptr;
        });
        return true;
    }();
    Q_UNUSED(installed);
}

// Launch an external helper (a kcmshell6 module, kfontview, a settings applet…)
// detached. If the program isn't on PATH, tell the user rather than failing
// silently, the same courtesy MainWindow extends to its own command links.
inline void launchDetached(QWidget *parent, const QStringList &cmd)
{
    if (cmd.isEmpty())
        return;
    if (isAeroPolicyBlockedCommand(cmd)) {
        QMessageBox::information(parent, QStringLiteral("Aero Personalization"), aeroPolicyCommandReason());
        return;
    }
    QStringList args = cmd;
    const QString program = args.takeFirst();
    if (QStandardPaths::findExecutable(program).isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("Control Panel"),
            QStringLiteral("\"%1\" is not installed.").arg(program));
        return;
    }
    QProcess::startDetached(program, args);
}
