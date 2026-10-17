#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTabWidget>
#include <QDir>
#include <AeroQt/stylesheet.h>
#include "ui/pages/InputDevicesPage.h"
#include "settings/InputSettings.h"
class PageInputBackend final : public InputBackend {
public:
    int writes = 0;
    InputSnapshot value;
    PageInputBackend() {
        value.available = value.writable = true; value.owner = ":1.2"; value.version = "6.6.6";
        value.devices = {{"event1", "Aero Test Mouse", "/org/kde/KWin/InputDevice/event1", {
            {"name", "Aero Test Mouse"}, {"sysName", "event1"}, {"vendor", 1u}, {"product", 2u},
            {"pointer", true}, {"touchpad", false}, {"isVirtual", false},
            {"leftHanded", false}, {"supportsLeftHanded", true}, {"leftHandedEnabledByDefault", false},
            {"pointerAcceleration", 0.0}, {"supportsPointerAcceleration", true}, {"defaultPointerAcceleration", 0.0},
            {"scrollFactor", 1.0}, {"naturalScroll", false}, {"supportsNaturalScroll", false},
            {"__writable", QStringList{"leftHanded", "pointerAcceleration", "scrollFactor", "naturalScroll"}}
        }}};
    }
    void readCurrent(quint64 token) override { emit readFinished(token, true, value, {}); }
    void writeChanges(quint64 token, const InputSnapshot &, const InputSnapshot &requested) override {
        ++writes; value = requested; emit writeFinished(token, InputOutcome::Success, value, {});
    }
};
static void captureInput(QWidget &widget, const QString &name)
{
    const QString directory = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    QDir().mkpath(directory); widget.resize(1050, 790); widget.show(); QApplication::processEvents();
    QVERIFY(widget.grab().save(directory + QLatin1Char('/') + name + QStringLiteral(".png")));
}
class InputPageTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR")) Aero::registerStylesheet(qApp); }
    void nativeControlsStageAndSurviveViewRecreation()
    {
        auto *backend = new PageInputBackend; InputSettings session(backend); QTRY_VERIFY(session.isReady());
        auto *page = new InputDevicesPage(nullptr, &session, nullptr);
        page->show(); QApplication::processEvents();
        captureInput(*page, QStringLiteral("mouse-properties"));
        auto *check = page->findChild<QCheckBox *>("input_leftHanded"); QVERIFY(check); QVERIFY(!check->accessibleName().isEmpty());
        check->setChecked(true); QCOMPARE(backend->writes, 0); QVERIFY(session.isDirty());
        auto *speed = page->findChild<QDoubleSpinBox *>("input_pointerAcceleration"); QVERIFY(speed); speed->setValue(0.4);
        QVERIFY(page->findChild<QPushButton *>("inputApply")->isEnabled()); delete page;
        page = new InputDevicesPage(nullptr, &session, nullptr);
        QCOMPARE(page->settingsSession(), static_cast<SettingsSession *>(&session));
        QVERIFY(page->findChild<QCheckBox *>("input_leftHanded")->isChecked());
        QCOMPARE(page->findChild<QDoubleSpinBox *>("input_pointerAcceleration")->value(), 0.4);
        page->findChild<QPushButton *>("inputApply")->click(); QCOMPARE(backend->writes, 1); QVERIFY(!session.isDirty());
        delete page;
    }
    void touchpadModesTappingAndDefaultsAreNativeAndStaged()
    {
        auto *backend = new PageInputBackend;
        auto &p = backend->value.devices[0].properties; p["touchpad"] = true;
        p["tapFingerCount"] = 3; p["tapToClick"] = false; p["tapToClickEnabledByDefault"] = false;
        p["scrollTwoFinger"] = true; p["scrollEdge"] = false; p["supportsScrollTwoFinger"] = true; p["supportsScrollEdge"] = true;
        p["scrollTwoFingerEnabledByDefault"] = true; p["scrollEdgeEnabledByDefault"] = false;
        p["clickMethodAreas"] = true; p["clickMethodClickfinger"] = false;
        p["supportsClickMethodAreas"] = true; p["supportsClickMethodClickfinger"] = true;
        p["defaultClickMethodAreas"] = true; p["defaultClickMethodClickfinger"] = false;
        auto writable = p["__writable"].toStringList(); writable << "tapToClick" << "scrollTwoFinger" << "scrollEdge" << "clickMethodAreas" << "clickMethodClickfinger"; p["__writable"] = writable;
        InputSettings session(backend); QTRY_VERIFY(session.isReady());
        InputDevicesPage page(nullptr, &session, nullptr); page.show(); QApplication::processEvents();
        auto *click = page.findChild<QComboBox *>("inputClickMethod"); QVERIFY(click->isVisible()); QVERIFY(click->isEnabled());
        click->setCurrentIndex(click->findData("clickMethodClickfinger"));
        QVERIFY(session.staged().devices[0].properties["clickMethodClickfinger"].toBool());
        QVERIFY(!session.staged().devices[0].properties["clickMethodAreas"].toBool());
        page.findChild<QTabWidget *>("inputPropertiesTabs")->setCurrentIndex(3);
        auto *tap = page.findChild<QCheckBox *>("input_tapToClick"); QVERIFY(tap->isVisible()); QVERIFY(tap->isEnabled()); QVERIFY(!tap->accessibleName().isEmpty());
        tap->setChecked(true); QVERIFY(session.staged().devices[0].properties["tapToClick"].toBool());
        captureInput(page, QStringLiteral("touchpad-tapping"));
        page.findChild<QTabWidget *>("inputPropertiesTabs")->setCurrentIndex(2);
        auto *scroll = page.findChild<QComboBox *>("inputScrollMethod"); scroll->setCurrentIndex(scroll->findData("scrollEdge"));
        QVERIFY(session.staged().devices[0].properties["scrollEdge"].toBool()); QVERIFY(!session.staged().devices[0].properties["scrollTwoFinger"].toBool());
        captureInput(page, QStringLiteral("touchpad-scrolling"));
        QCOMPARE(backend->writes, 0); page.findChild<QPushButton *>("inputDefaults")->click();
        QVERIFY(!session.staged().devices[0].properties["tapToClick"].toBool());
        QVERIFY(session.staged().devices[0].properties["clickMethodAreas"].toBool());
        QVERIFY(session.staged().devices[0].properties["scrollTwoFinger"].toBool()); QCOMPARE(backend->writes, 0);
    }
    void unavailableControlsCannotStageAppPreferences()
    {
        auto *backend = new PageInputBackend; InputSettings session(backend); QTRY_VERIFY(session.isReady());
        InputDevicesPage page(nullptr, &session, nullptr);
        page.show(); QApplication::processEvents();
        QVERIFY(!page.findChild<QCheckBox *>("input_naturalScroll")->isEnabled());
        QVERIFY(!page.findChild<QComboBox *>("inputClickMethod")->isVisible());
        QVERIFY(!page.findChild<QPushButton *>("inputApply")->isEnabled());
        QCOMPARE(backend->writes, 0);
    }
};
QTEST_MAIN(InputPageTest)
#include "input_page_test.moc"
