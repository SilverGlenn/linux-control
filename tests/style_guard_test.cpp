#include "ui/ApplicationStyleGuard.h"
#include <QtTest>
#include <QProxyStyle>
#include <QStyleFactory>

class StyleGuardTest final : public QObject {
    Q_OBJECT
private slots:
    void applicationIconsRemainPinnedAfterThemeNotifications() {
        const QString oldTheme = QIcon::themeName();
        auto *guard = new ApplicationStyleGuard(qApp, [](QApplication *app) {
            app->setStyle(QStyleFactory::create(QStringLiteral("fusion")));
        }, QStringLiteral("Windows 7 Aero"));
        QCOMPARE(QIcon::themeName(), QStringLiteral("Windows 7 Aero"));
        QWidget window;
        QIcon::setThemeName(QStringLiteral("other"));
        QEvent event(QEvent::ThemeChange);
        QCoreApplication::sendEvent(&window, &event);
        QTRY_COMPARE(QIcon::themeName(), QStringLiteral("Windows 7 Aero"));
        delete guard;
        QIcon::setThemeName(oldTheme);
    }
    void externalStyleChangesKeepExplicitApplicationPresentation() {
        int installs = 0;
        auto *guard = new ApplicationStyleGuard(qApp, [&](QApplication *app) {
            ++installs;
            auto *style = new QProxyStyle(QStyleFactory::create(QStringLiteral("fusion")));
            style->setProperty("controlPanelStyle", true); app->setStyle(style);
        });
        QWidget window; window.show(); QCOMPARE(installs, 1);
        qApp->setStyle(QStyleFactory::create(QStringLiteral("windows")));
        QTRY_COMPARE(installs, 2); QVERIFY(qApp->style()->property("controlPanelStyle").toBool());
        const QPalette palette = qApp->palette(); qApp->setPalette(palette);
        QTest::qWait(30); QCOMPARE(installs, 2);
        qApp->setStyle(QStyleFactory::create(QStringLiteral("fusion")));
        QTRY_COMPARE(installs, 3); QTest::qWait(30); QCOMPARE(installs, 3);
        delete guard;
    }
};
QTEST_MAIN(StyleGuardTest)
#include "style_guard_test.moc"
