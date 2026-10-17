#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include "PersonalizationPage.h"
#include "ui/pages/ThemeComponentsPage.h"
#include "settings/AeroThemeSettings.h"
#include "settings/AeroThemeContract.h"

class PersonalizationBackend final : public AeroThemeBackend {
public:
    int reads = 0, writes = 0;
    QVariantMap expected, requested;
    void readCurrent() override { ++reads; }
    void writeSettings(const QVariantMap &old, const QVariantMap &next) override { ++writes; expected = old; requested = next; }
    static AeroThemeState available(int intensity = 60)
    {
        AeroThemeState state; state.values = AeroThemeContract::defaults(); state.values["aeroIntensity"] = intensity;
        for (auto it = state.values.cbegin(); it != state.values.cend(); ++it) state.writableKeys.insert(it.key());
        state.assetsSupported = state.runtimeSupported = true; return state;
    }
    void report(const AeroThemeState &state = available()) { emit readFinished(true, state, {}); }
    void written(bool success = true) { emit writeFinished(success, success ? QString() : QStringLiteral("Saved partially; runtime reload failed")); }
};
class PersonalizationTest final : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        backend = new PersonalizationBackend; session = new AeroThemeSettings(backend);
        QTRY_COMPARE(backend->reads, 1); backend->report();
    }
    void cleanup() { delete session; }
    void nativeKeyboardColorControlsStageWithoutWriting()
    {
        PersonalizationPage page(nullptr, session, nullptr); page.show();
        auto *color = page.findChild<QPushButton *>("aeroColorSwatch1"); QVERIFY(color);
        QCOMPARE(color->focusPolicy(), Qt::StrongFocus); QVERIFY(!color->accessibleName().isEmpty());
        QTest::keyClick(color, Qt::Key_Space); QVERIFY(session->isDirty());
        QVERIFY(color->isChecked());
        QVERIFY(session->stagedValue("aeroHue").toInt() > 0); QCOMPARE(backend->writes, 0);
        auto *intensity = page.findChild<QSpinBox *>("aeroIntensity"); QVERIFY(intensity); QVERIFY(!intensity->accessibleName().isEmpty());
        intensity->setValue(80); QCOMPARE(session->stagedValue("aeroIntensity").toInt(), 80); QCOMPARE(session->appliedValue("aeroIntensity").toInt(), 60);
        auto *transparency = page.findChild<QCheckBox *>("enableTransparency"); QVERIFY(transparency);
        QTest::keyClick(transparency, Qt::Key_Space); QVERIFY(!session->stagedValue("enableTransparency").toBool());
        QVERIFY(page.findChild<QPushButton *>("applyAeroTheme")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("aeroThemeReview")->text().contains("Glass color intensity"));
        QVERIFY(page.findChild<QLabel *>("aeroThemeStatus")->text().contains("not been applied"));
        QCOMPARE(backend->writes, 0);
    }
    void viewsShareStagingAndHaveNoAlternativeThemeSelectors()
    {
        auto *page = new PersonalizationPage(nullptr, session, nullptr);
        page->findChild<QSpinBox *>("aeroIntensity")->setValue(80); delete page;
        ThemeComponentsPage components(nullptr, session, nullptr);
        QCOMPARE(components.settingsSession(), static_cast<SettingsSession *>(session));
        QVERIFY(!components.findChild<QComboBox *>("iconTheme")); QVERIFY(!components.findChild<QComboBox *>("cursorTheme"));
        QVERIFY(!components.findChild<QComboBox *>("applicationStyle")); QVERIFY(components.findChildren<QComboBox *>().isEmpty());
        QVERIFY(components.findChild<QLabel *>("aeroComponentsReview")->text().contains("80"));
        PersonalizationPage replacement(nullptr, session, nullptr);
        QCOMPARE(replacement.findChild<QSpinBox *>("aeroIntensity")->value(), 80);
        QCOMPARE(replacement.settingsSession(), components.settingsSession()); QCOMPARE(backend->writes, 0);
    }
    void defaultsButtonStagesAndExplainsPreservedScope()
    {
        PersonalizationPage page(nullptr, session, nullptr);
        auto *restore = page.findChild<QPushButton *>("restoreAeroDefaults"); QVERIFY(restore); restore->click();
        QCOMPARE(backend->writes, 0); QVERIFY(session->isDirty()); QCOMPARE(session->stagedValues(), AeroThemeContract::defaults());
        QVERIFY(page.findChild<QLabel *>("aeroThemeMessage")->text().contains("staged"));
        QString allText; for (auto *label : page.findChildren<QLabel *>()) allText += label->text();
        QVERIFY(allText.contains("Wallpaper, fonts, panel layout, and shortcuts are preserved"));
        QVERIFY(page.findChild<QPushButton *>("applyAeroTheme")->isEnabled());
        page.findChild<QPushButton *>("revertAeroTheme")->click(); backend->report();
        QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
    }
    void pendingApplyOutlivesBothViewsAndFinishesAfterReadback()
    {
        auto *page = new PersonalizationPage(nullptr, session, nullptr);
        page->findChild<QSpinBox *>("aeroIntensity")->setValue(80);
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        page->findChild<QPushButton *>("applyAeroTheme")->click(); backend->report(); QCOMPARE(backend->writes, 1);
        QVERIFY(!page->findChild<QSpinBox *>("aeroIntensity")->isEnabled()); delete page;
        auto *components = new ThemeComponentsPage(nullptr, session, nullptr);
        QVERIFY(!components->findChild<QPushButton *>("applyAeroTheme")->isEnabled()); delete components;
        backend->written(); QVERIFY(finished.isEmpty());
        auto saved = PersonalizationBackend::available(80); saved.values = backend->requested; backend->report(saved);
        QCOMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool()); QVERIFY(!session->isDirty());
        PersonalizationPage replacement(nullptr, session, nullptr);
        QVERIFY(replacement.findChild<QLabel *>("aeroThemeMessage")->text().contains("readback"));
    }
    void driftAndMissingAssetsAreShownWithoutCoercingSavedValues()
    {
        auto state = PersonalizationBackend::available(); state.values["iconTheme"] = "OtherIcons";
        state.values["cursorSize"] = 36; state.assetsSupported = false; state.issues << "Aero assets differ from the verified release";
        session->refresh(); backend->report(state);
        PersonalizationPage page(nullptr, session, nullptr); ThemeComponentsPage components(nullptr, session, nullptr);
        QCOMPARE(page.findChild<QComboBox *>("cursorSize")->currentData().toInt(), 36);
        QVERIFY(!page.findChild<QSpinBox *>("aeroIntensity")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("aeroThemeMessage")->text().contains("assets differ"));
        QVERIFY(components.findChild<QLabel *>("iconThemeStatus")->text().contains("OtherIcons"));
        QVERIFY(components.findChild<QLabel *>("iconThemeStatus")->text().contains("Differs from approved configuration"));
        QCOMPARE(session->appliedValue("cursorSize").toInt(), 36); QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
        components.findChild<QPushButton *>("restoreAeroDefaults")->click(); QVERIFY(session->isDirty());
        QVERIFY(!components.findChild<QPushButton *>("applyAeroTheme")->isEnabled()); QCOMPARE(backend->writes, 0);
    }
    void partialFailureShowsSavedAndStagedValuesAndRequiresRevert()
    {
        PersonalizationPage page(nullptr, session, nullptr); ThemeComponentsPage components(nullptr, session, nullptr);
        session->selectValue("aeroIntensity", 80); session->selectValue("cursorSize", 48);
        session->apply(); backend->report(); backend->written(false); backend->report(PersonalizationBackend::available(80));
        QVERIFY(page.findChild<QLabel *>("aeroThemeMessage")->text().contains("Partial or unverified"));
        QVERIFY(page.findChild<QLabel *>("cursorSizeSaved")->text().contains("Selected: 48"));
        QVERIFY(components.findChild<QLabel *>("aeroComponentsMessage")->text().contains("reload failed"));
        QVERIFY(!page.findChild<QPushButton *>("applyAeroTheme")->isEnabled());
        QVERIFY(components.findChild<QPushButton *>("revertAeroTheme")->isEnabled());
        components.findChild<QPushButton *>("revertAeroTheme")->click(); backend->report(PersonalizationBackend::available(80));
        QVERIFY(!session->hasConflict()); QVERIFY(!session->isDirty());
    }
private:
    PersonalizationBackend *backend = nullptr;
    AeroThemeSettings *session = nullptr;
};
QTEST_MAIN(PersonalizationTest)
#include "personalization_test.moc"
