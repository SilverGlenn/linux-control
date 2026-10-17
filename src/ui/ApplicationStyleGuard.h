#pragma once
#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QStyle>
#include <QIcon>
#include <QTimer>
#include <functional>

// KDE broadcasts widget-style changes to all applications, including this
// editor. Preserve its explicit Windows 7 presentation after those broadcasts.
// Only this process's style is restored; the saved desktop preference is kept.
class ApplicationStyleGuard final : public QObject {
public:
    using Installer = std::function<void(QApplication *)>;
    ApplicationStyleGuard(QApplication *application, Installer installer, const QString &iconTheme = {})
        : QObject(application), m_app(application), m_install(std::move(installer)), m_iconTheme(iconTheme)
    {
        restore();
        restoreIcons();
        application->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::StyleChange && m_app->style() != m_expected) schedule();
        if ((event->type() == QEvent::ThemeChange || event->type() == QEvent::StyleChange)
            && !m_iconTheme.isEmpty() && QIcon::themeName() != m_iconTheme) schedule();
        return QObject::eventFilter(watched, event);
    }
private:
    void schedule()
    {
        if (m_pending || m_restoring || QCoreApplication::closingDown()) return;
        m_pending = true;
        QTimer::singleShot(0, this, [this] {
            m_pending = false;
            if (!QCoreApplication::closingDown()) {
                if (m_app->style() != m_expected) restore();
                restoreIcons();
            }
        });
    }
    void restore()
    {
        m_restoring = true;
        disconnect(m_destroyed);
        m_install(m_app);
        m_expected = m_app->style();
        if (m_expected) m_destroyed = connect(m_expected, &QObject::destroyed, this, [this] { schedule(); });
        m_restoring = false;
    }
    void restoreIcons()
    {
        if (!m_iconTheme.isEmpty() && QIcon::themeName() != m_iconTheme)
            QIcon::setThemeName(m_iconTheme);
    }
    QApplication *m_app;
    Installer m_install;
    QString m_iconTheme;
    QPointer<QStyle> m_expected;
    QMetaObject::Connection m_destroyed;
    bool m_pending = false, m_restoring = false;
};
