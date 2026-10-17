#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPointer>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <AeroQt/stylesheet.h>
#include "settings/KeyboardSettings.h"
#include "ui/pages/KeyboardPage.h"
#include "keyboard/KeyboardPreview.h"

class PageKeyboardBackend final : public KeyboardBackend {
public:
    KeyboardSnapshot value;
    QVariantMap requested;
    int reads = 0, writes = 0;
    bool holdWrite = false;
    quint64 pending = 0;
    void readCurrent(quint64 token) override { ++reads; emit readFinished(token, true, value, {}); }
    void writePreferences(quint64 token, const KeyboardSnapshot &, const QVariantMap &next) override {
        ++writes; requested = next; pending = token;
        if (!holdWrite) finish();
    }
    void finish() {
        value.values = requested; value.revision = QByteArrayLiteral("saved");
        value.runtimeStatus = QStringLiteral("Preferences saved. Reload requested; runtime adoption is unverified.");
        emit writeFinished(pending, KeyboardOutcome::Success, value, {});
    }
    void changed() { emit externalChange(); }
};

static QTreeWidgetItem *option(QTreeWidget *tree, const QString &id)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        for (int j = 0; j < tree->topLevelItem(i)->childCount(); ++j) {
            auto *item = tree->topLevelItem(i)->child(j);
            if (item->data(0, Qt::UserRole).toString() == id) return item;
        }
    return nullptr;
}
static void captureKeyboard(QWidget &page, const QString &name)
{
    const auto directory = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    QDir().mkpath(directory); page.resize(1050, 850); page.show(); QApplication::processEvents();
    QVERIFY(page.grab().save(directory + QLatin1Char('/') + name + QStringLiteral(".png")));
}

class KeyboardPageTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR")) Aero::registerStylesheet(qApp);
        QVERIFY(directory.isValid());
        QFile fixture(directory.filePath(QStringLiteral("evdev.xml"))); QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write(R"XML(<?xml version="1.0"?>
<xkbConfigRegistry version="1.1">
 <modelList>
  <model><configItem><name>pc104</name><description>Generic 104-key PC</description></configItem></model>
  <model><configItem><name>pc105</name><description>Generic 105-key PC</description></configItem></model>
 </modelList>
 <layoutList>
  <layout><configItem><name>us</name><description>English (US)</description></configItem><variantList>
   <variant><configItem><name>intl</name><description>English (US, international)</description></configItem></variant>
  </variantList></layout>
  <layout><configItem><name>de</name><description>German</description></configItem><variantList>
   <variant><configItem><name>nodeadkeys</name><description>German (no dead keys)</description></configItem></variant>
  </variantList></layout>
  <layout><configItem><name>gb</name><description>English (UK)</description></configItem></layout>
 </layoutList>
 <optionList>
  <group allowMultipleSelection="false"><configItem><name>ctrl</name><description>Ctrl key position</description></configItem>
   <option><configItem><name>ctrl:nocaps</name><description>Caps Lock as Ctrl</description></configItem></option>
   <option><configItem><name>ctrl:swapcaps</name><description>Swap Ctrl and Caps Lock</description></configItem></option>
  </group>
  <group allowMultipleSelection="true"><configItem><name>grp</name><description>Switching to another layout</description></configItem>
   <option><configItem><name>grp:alt_shift_toggle</name><description>Alt+Shift</description></configItem></option>
   <option><configItem><name>grp:win_space_toggle</name><description>Win+Space</description></configItem></option>
  </group>
 </optionList>
</xkbConfigRegistry>)XML");
        fixture.close(); registry = KeyboardTypes::readRegistry(fixture.fileName());
        QVERIFY2(registry.valid, qPrintable(registry.error));
    }
    void init()
    {
        backend = new PageKeyboardBackend;
        backend->value.available = backend->value.writable = backend->value.reloadAvailable = true;
        backend->value.owner = QStringLiteral(":1.2"); backend->value.version = QStringLiteral("6.6.6");
        backend->value.revision = QByteArrayLiteral("initial"); backend->value.registry = registry;
        backend->value.values = KeyboardTypes::defaults();
        backend->value.values["Model"] = QStringLiteral("pc104"); backend->value.values["Use"] = true;
        backend->value.values["KeyRepeat"] = QStringLiteral("repeat");
        backend->value.values["ResetOldOptions"] = true;
        backend->value.values["LayoutList"] = QStringList{"us", "de"};
        backend->value.values["VariantList"] = QStringList{"", "nodeadkeys"};
        backend->value.values["DisplayNames"] = QStringList{"US", "DE"};
        backend->value.values["Options"] = QStringList{"ctrl:nocaps"};
        session = new KeyboardSettings(backend); QTRY_VERIFY(session->isReady());
    }
    void cleanup() { delete session; }
    void nativeSpeedChoicesStageAndSurvivePageRecreation()
    {
        auto *page = new KeyboardPage(nullptr, session);
        QCOMPARE(page->settingsSession(), static_cast<SettingsSession *>(session));
        auto *delay = page->findChild<QSpinBox *>("keyboardRepeatDelay"); QVERIFY(delay);
        QVERIFY(!delay->accessibleName().isEmpty()); delay->setValue(750);
        auto *rate = page->findChild<QDoubleSpinBox *>("keyboardRepeatRate"); rate->setValue(31.5);
        auto *lock = page->findChild<QComboBox *>("keyboardNumLock"); lock->setCurrentIndex(lock->findData(1));
        QCOMPARE(backend->writes, 0); QVERIFY(session->isDirty()); captureKeyboard(*page, QStringLiteral("keyboard-speed")); delete page;
        page = new KeyboardPage(nullptr, session);
        QCOMPARE(page->findChild<QSpinBox *>("keyboardRepeatDelay")->value(), 750);
        QCOMPARE(page->findChild<QDoubleSpinBox *>("keyboardRepeatRate")->value(), 31.5);
        QCOMPARE(page->findChild<QComboBox *>("keyboardNumLock")->currentData().toInt(), 1);
        QVERIFY(page->findChild<QPushButton *>("keyboardApply")->isEnabled());
        page->findChild<QPushButton *>("keyboardApply")->click(); QTRY_COMPARE(backend->writes, 1); QTRY_VERIFY(!session->isDirty());
        QVERIFY(page->findChild<QLabel *>("keyboardRuntimeStatus")->text().contains("unverified")); delete page;
    }
    void previewSnapshotsStagedSelectionWithoutApplying()
    {
        auto *page = new KeyboardPage(nullptr, session);
        page->findChild<QTabWidget *>("keyboardPropertiesTabs")->setCurrentIndex(1);
        auto *layouts = page->findChild<QTreeWidget *>("keyboardLayouts");
        layouts->setCurrentItem(layouts->topLevelItem(1));
        session->selectValue(QStringLiteral("Model"), QStringLiteral("pc105"));
        const auto staged = session->staged().values;
        auto *preview = page->findChild<QPushButton *>("keyboardPreviewLayout"); QVERIFY(preview);
        QVERIFY(preview->isEnabled()); QVERIFY(!preview->accessibleName().isEmpty()); preview->click();
        auto *dialog = page->findChild<KeyboardPreviewDialog *>(); QVERIFY(dialog);
        QPointer<KeyboardPreviewDialog> guard(dialog);
        QTRY_VERIFY(!dialog->result().keys.isEmpty() || !dialog->result().error.isEmpty());
        QVERIFY2(dialog->result().valid(), qPrintable(dialog->result().error));
        QCOMPARE(dialog->result().request.model, QStringLiteral("pc105"));
        QCOMPARE(dialog->result().request.layout, QStringLiteral("de"));
        QCOMPARE(dialog->result().request.variant, QStringLiteral("nodeadkeys"));
        QCOMPARE(dialog->result().request.options, (QStringList{"ctrl:nocaps"}));
        preview->click(); QCOMPARE(page->findChildren<KeyboardPreviewDialog *>().size(), 1);
        QCOMPARE(backend->writes, 0); QCOMPARE(session->staged().values, staged);
        dialog->findChild<QPushButton *>("keyboardPreviewClose")->click(); QTRY_VERIFY(guard.isNull());
        preview->click(); auto *reopened = page->findChild<KeyboardPreviewDialog *>(); QVERIFY(reopened);
        QTRY_VERIFY(!reopened->result().keys.isEmpty() || !reopened->result().error.isEmpty());
        delete page; // The dialog belongs to its page, independently of the settings session.
        QCOMPARE(backend->writes, 0); QCOMPARE(session->staged().values, staged);
    }
    void unknownSavedLayoutCanExplainPreviewFailure()
    {
        backend->value.values["LayoutList"] = QStringList{"not_installed_saved_layout"};
        backend->value.values["VariantList"] = QStringList{""}; backend->changed();
        KeyboardPage page(nullptr, session);
        auto *preview = page.findChild<QPushButton *>("keyboardPreviewLayout"); QVERIFY(preview->isEnabled());
        preview->click(); auto *dialog = page.findChild<KeyboardPreviewDialog *>(); QVERIFY(dialog);
        QTRY_VERIFY(!dialog->result().error.isEmpty());
        QVERIFY(dialog->result().error.contains("not_installed_saved_layout"));
        QCOMPARE(backend->writes, 0); QVERIFY(!session->isDirty());
    }
    void savedFractionalRateIsDisplayedWithoutRoundingOrStaging()
    {
        backend->value.values["RepeatRate"] = 0.25; backend->changed();
        KeyboardPage page(nullptr, session);
        auto *rate = page.findChild<QDoubleSpinBox *>("keyboardRepeatRate"); QVERIFY(rate);
        QCOMPARE(rate->decimals(), 2); QCOMPARE(rate->value(), 0.25);
        QVERIFY(rate->text().contains(rate->locale().toString(0.25, 'f', 2)));
        QCOMPARE(session->staged().values["RepeatRate"].toDouble(), 0.25);
        QVERIFY(!session->isDirty()); QCOMPARE(backend->writes, 0);
        page.findChild<QSpinBox *>("keyboardRepeatDelay")->setValue(750);
        page.findChild<QPushButton *>("keyboardApply")->click(); QTRY_COMPARE(backend->writes, 1);
        QCOMPARE(backend->requested["RepeatRate"].toDouble(), 0.25);
    }
    void layoutVariantsLabelsOrderAndPolicyStageAtomically()
    {
        KeyboardPage page(nullptr, session);
        auto *tabs = page.findChild<QTabWidget *>("keyboardPropertiesTabs"); tabs->setCurrentIndex(1);
        auto *choice = page.findChild<QComboBox *>("keyboardLayoutChoice"); choice->setCurrentIndex(choice->findData("us"));
        auto *variant = page.findChild<QComboBox *>("keyboardVariantChoice"); variant->setCurrentIndex(variant->findData("intl"));
        page.findChild<QPushButton *>("keyboardAddLayout")->click();
        QCOMPARE(session->staged().values["LayoutList"].toStringList(), (QStringList{"us", "de", "us"}));
        QCOMPARE(session->staged().values["VariantList"].toStringList(), (QStringList{"", "nodeadkeys", "intl"}));
        auto *label = page.findChild<QLineEdit *>("keyboardDisplayLabel"); label->setFocus(); QTest::keyClicks(label, "INT");
        QCOMPARE(session->staged().values["DisplayNames"].toStringList().last(), "INT");
        page.findChild<QPushButton *>("keyboardLayoutUp")->click();
        QCOMPARE(session->staged().values["VariantList"].toStringList(), (QStringList{"", "intl", "nodeadkeys"}));
        QCOMPARE(session->staged().values["DisplayNames"].toStringList(), (QStringList{"US", "INT", "DE"}));
        auto *policy = page.findChild<QComboBox *>("keyboardSwitchingPolicy"); policy->setCurrentIndex(policy->findData("Window"));
        QCOMPARE(session->staged().values["SwitchMode"].toString(), "Window");
        captureKeyboard(page, QStringLiteral("keyboard-layouts"));
        page.findChild<QPushButton *>("keyboardRemoveLayout")->click();
        QCOMPARE(session->staged().values["LayoutList"].toStringList(), (QStringList{"us", "de"}));
        QCOMPARE(backend->writes, 0);
    }
    void addLayoutCanEnableConfigurationAfterTurningItOff()
    {
        KeyboardPage page(nullptr, session);
        page.findChild<QCheckBox *>("keyboardConfigureLayouts")->setChecked(false);
        QVERIFY(!session->staged().values["Use"].toBool());
        QVERIFY(session->staged().values["LayoutList"].toStringList().isEmpty());
        auto *choice = page.findChild<QComboBox *>("keyboardLayoutChoice"); QVERIFY(choice->isEnabled());
        choice->setCurrentIndex(choice->findData("de"));
        auto *variant = page.findChild<QComboBox *>("keyboardVariantChoice"); variant->setCurrentIndex(variant->findData("nodeadkeys"));
        auto *add = page.findChild<QPushButton *>("keyboardAddLayout"); QVERIFY(add->isEnabled()); add->click();
        QVERIFY(session->staged().values["Use"].toBool());
        QCOMPARE(session->staged().values["LayoutList"].toStringList(), (QStringList{"de"}));
        QCOMPARE(session->staged().values["VariantList"].toStringList(), (QStringList{"nodeadkeys"}));
        QCOMPARE(backend->writes, 0);
    }
    void layoutNotificationStagesSurvivesRecreationAndApplies()
    {
        backend->value.values["kbdLayoutChangedEnabled"] = true; backend->changed();
        auto *page = new KeyboardPage(nullptr, session);
        auto *popup = page->findChild<QCheckBox *>("keyboardLayoutOsd"); QVERIFY(popup);
        QVERIFY(!popup->accessibleName().isEmpty()); QVERIFY(popup->isChecked());
        popup->setChecked(false);
        QCOMPARE(session->staged().values["kbdLayoutChangedEnabled"].toBool(), false);
        QCOMPARE(backend->writes, 0); QVERIFY(session->isDirty()); delete page;
        page = new KeyboardPage(nullptr, session);
        QVERIFY(!page->findChild<QCheckBox *>("keyboardLayoutOsd")->isChecked());
        page->findChild<QPushButton *>("keyboardApply")->click(); QTRY_COMPARE(backend->writes, 1);
        QTRY_VERIFY(!session->isDirty());
        QCOMPARE(backend->requested["kbdLayoutChangedEnabled"].toBool(), false);
        delete page;
    }
    void groupedOptionsRespectSingleAndMultipleChoice()
    {
        KeyboardPage page(nullptr, session);
        page.findChild<QTabWidget *>("keyboardPropertiesTabs")->setCurrentIndex(2);
        auto *tree = page.findChild<QTreeWidget *>("keyboardOptions");
        QVERIFY(option(tree, "ctrl:nocaps")); QVERIFY(option(tree, "ctrl:swapcaps"));
        option(tree, "ctrl:swapcaps")->setCheckState(0, Qt::Checked);
        QVERIFY(!session->staged().values["Options"].toStringList().contains("ctrl:nocaps"));
        QVERIFY(option(tree, "ctrl:nocaps")->checkState(0) == Qt::Unchecked);
        option(tree, "grp:alt_shift_toggle")->setCheckState(0, Qt::Checked);
        option(tree, "grp:win_space_toggle")->setCheckState(0, Qt::Checked);
        const auto selected = session->staged().values["Options"].toStringList();
        QVERIFY(selected.contains("grp:alt_shift_toggle")); QVERIFY(selected.contains("grp:win_space_toggle"));
        tree->expandAll(); captureKeyboard(page, QStringLiteral("keyboard-advanced"));
        QCOMPARE(backend->writes, 0);
        page.findChild<QPushButton *>("keyboardDefaults")->click(); QCOMPARE(backend->writes, 0);
        page.findChild<QPushButton *>("keyboardRevert")->click(); QTRY_VERIFY(!session->isDirty());
        QCOMPARE(session->staged().values["Options"].toStringList(), (QStringList{"ctrl:nocaps"}));
    }
    void applyJobSurvivesPageDestruction()
    {
        backend->holdWrite = true;
        auto *page = new KeyboardPage(nullptr, session);
        page->findChild<QSpinBox *>("keyboardRepeatDelay")->setValue(800);
        page->findChild<QPushButton *>("keyboardApply")->click(); QTRY_COMPARE(backend->writes, 1);
        QVERIFY(session->isBusy()); delete page;
        KeyboardPage replacement(nullptr, session);
        QVERIFY(!replacement.findChild<QPushButton *>("keyboardApply")->isEnabled());
        backend->finish(); QTRY_VERIFY(!session->isBusy()); QVERIFY(!session->isDirty());
        QCOMPARE(replacement.findChild<QSpinBox *>("keyboardRepeatDelay")->value(), 800);
    }
    void refreshPreservesTypedTextAndImmutableSettingsStayDisabled()
    {
        backend->value.immutableKeys.insert("LayoutLoopCount"); backend->changed();
        KeyboardPage page(nullptr, session); page.show(); QApplication::processEvents();
        QVERIFY(!page.findChild<QSpinBox *>("keyboardLayoutLoop")->isEnabled());
        auto *delay = page.findChild<QSpinBox *>("keyboardRepeatDelay"); delay->setFocus();
        auto *edit = delay->findChild<QLineEdit *>(); QVERIFY(edit);
        edit->selectAll(); QTest::keyClicks(edit, "7"); const auto typed = edit->text();
        backend->value.runtimeStatus = QStringLiteral("Runtime layout information refreshed."); backend->changed();
        QTRY_VERIFY(page.findChild<QLabel *>("keyboardRuntimeStatus")->text().contains("refreshed"));
        QCOMPARE(edit->text(), typed); QCOMPARE(backend->writes, 0);
    }
    void unavailableSavedChoicesRemainVisible()
    {
        backend->value.values["Model"] = QStringLiteral("custom_model");
        backend->value.values["LayoutList"] = QStringList{"custom"};
        backend->value.values["VariantList"] = QStringList{"custom_variant"};
        backend->value.values["DisplayNames"] = QStringList{"CUSTOM"};
        backend->value.values["Options"] = QStringList{"custom:option"}; backend->changed();
        KeyboardPage page(nullptr, session);
        QCOMPARE(page.findChild<QComboBox *>("keyboardModel")->currentData().toString(), "custom_model");
        auto *layouts = page.findChild<QTreeWidget *>("keyboardLayouts");
        QVERIFY(layouts->topLevelItem(0)->text(0).contains("custom"));
        QCOMPARE(page.findChild<QLineEdit *>("keyboardDisplayLabel")->text(), "CUSTOM");
        QVERIFY(page.findChild<QTreeWidget *>("keyboardOptions")->topLevelItemCount() > registry.optionGroups.size());
        QCOMPARE(backend->writes, 0); QVERIFY(!session->isDirty());
    }
private:
    QTemporaryDir directory;
    KeyboardRegistry registry;
    PageKeyboardBackend *backend = nullptr;
    KeyboardSettings *session = nullptr;
};
QTEST_MAIN(KeyboardPageTest)
#include "keyboard_page_test.moc"
