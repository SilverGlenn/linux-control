#include <QtTest>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <AeroQt/stylesheet.h>
#include "keyboard/KeyboardPreview.h"

namespace {
KeyboardPreview::Request request(const QString &layout = QStringLiteral("us"), const QString &variant = {}, const QStringList &options = {})
{ return {QStringLiteral("pc104"), layout, variant, options}; }
QString symbol(const KeyboardPreview::Result &result, const QString &key, int level = 0, int group = 0)
{
    const auto *entry = result.key(key);
    if (!entry || group >= entry->groups.size() || level >= entry->groups[group].levels.size()) return {};
    return entry->groups[group].levels[level].join(QStringLiteral(" + "));
}
}

class KeyboardPreviewTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR")) Aero::registerStylesheet(qApp);
    }
    void actualLayoutVariantAndLevels() {
        const auto us = KeyboardPreview::compile(request()); QVERIFY2(us.valid(), qPrintable(us.error));
        const auto de = KeyboardPreview::compile(request("de")); QVERIFY2(de.valid(), qPrintable(de.error));
        const auto colemak = KeyboardPreview::compile(request("us", "colemak")); QVERIFY2(colemak.valid(), qPrintable(colemak.error));
        QCOMPARE(symbol(us, "AD06"), QStringLiteral("y")); QCOMPARE(symbol(de, "AD06"), QStringLiteral("z"));
        QCOMPARE(symbol(us, "AD03"), QStringLiteral("e")); QCOMPARE(symbol(colemak, "AD03"), QStringLiteral("f"));
        QCOMPARE(symbol(us, "AE01", 1), QStringLiteral("!"));
        QCOMPARE(symbol(de, "AD01", 2), QStringLiteral("@"));
        QVERIFY(us.key("LFSH")); QCOMPARE(us.key("LFSH")->groups[0].modifiers[0], QStringLiteral("None"));
        QVERIFY(us.key("AE01")->groups[0].modifiers[1].contains("Shift"));
    }
    void deadKeysVariantsAndOptionsAreCompiled() {
        const auto de = KeyboardPreview::compile(request("de"));
        const auto plain = KeyboardPreview::compile(request("de", "nodeadkeys"));
        QVERIFY(de.valid()); QVERIFY(plain.valid());
        QCOMPARE(symbol(de, "AE12"), QStringLiteral("dead_acute")); QCOMPARE(symbol(plain, "AE12"), QStringLiteral("´"));
        const auto caps = KeyboardPreview::compile(request("us", {}, {"ctrl:nocaps"})); QVERIFY2(caps.valid(), qPrintable(caps.error));
        QCOMPARE(symbol(caps, "CAPS"), QStringLiteral("Ctrl"));
        const auto group = KeyboardPreview::compile(request("us", {}, {"grp:win_space_toggle"})); QVERIFY2(group.valid(), qPrintable(group.error));
        QVERIFY(group.key("LWIN")); QVERIFY(group.key("SPCE"));
        QVERIFY(group.keys.size() > group.geometry.size()); // Virtual/media keys remain inspectable.
    }
    void sourceGroundedAnsiAndIsoAreDifferent() {
        auto ansiRequest = request(); auto isoRequest = request(); isoRequest.model = "pc105";
        const auto ansi = KeyboardPreview::compile(ansiRequest), iso = KeyboardPreview::compile(isoRequest);
        QVERIFY(ansi.valid()); QVERIFY(iso.valid()); QCOMPARE(ansi.geometry.size(), 104); QCOMPARE(iso.geometry.size(), 105);
        auto shape = [](const KeyboardPreview::Result &r, const QString &name) {
            for (const auto &entry : r.geometry) if (entry.name == name) return entry.outline;
            return QPolygonF();
        };
        QCOMPARE(shape(ansi, "RTRN").boundingRect().height(), 18.0);
        QCOMPARE(shape(iso, "RTRN").boundingRect().height(), 37.0);
        QVERIFY(shape(ansi, "LSGT").isEmpty()); QVERIFY(!shape(iso, "LSGT").isEmpty());
        QCOMPARE(shape(iso, "LFSH").boundingRect().width(), 23.0);
        QCOMPARE(shape(ansi, "LFSH").boundingRect().width(), 42.0);
        for (const auto &entry : iso.geometry) QVERIFY2(iso.key(entry.name), qPrintable(entry.name));
    }
    void unsupportedGeometryDoesNotInventPhysicalPositions() {
        auto r = request(); r.model = "kinesis";
        const auto result = KeyboardPreview::compile(r); QVERIFY2(result.valid(), qPrintable(result.error));
        QVERIFY(result.geometry.isEmpty()); QVERIFY(result.geometryReason.contains("kinesis")); QVERIFY(result.geometryReason.contains("physical positions are not inferred"));
        QVERIFY(result.key("AD01")); QCOMPARE(symbol(result, "AD01"), QStringLiteral("q"));
    }
    void invalidSavedIdentifiersFailExplicitly_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("model") << QStringLiteral("model"); QTest::newRow("layout") << QStringLiteral("layout");
        QTest::newRow("variant") << QStringLiteral("variant"); QTest::newRow("option") << QStringLiteral("option");
    }
    void invalidSavedIdentifiersFailExplicitly() {
        QFETCH(QString, kind); auto r = request();
        if (kind == "model") r.model = "missing_saved_model";
        if (kind == "layout") r.layout = "missing_saved_layout";
        if (kind == "variant") r.variant = "missing_saved_variant";
        if (kind == "option") r.options = {"missing:saved_option"};
        const auto result = KeyboardPreview::compile(r); QVERIFY(!result.valid()); QVERIFY(!result.error.isEmpty());
        QVERIFY(result.geometry.isEmpty()); QVERIFY(result.keys.isEmpty());
    }
    void environmentCannotReplaceStagedNames() {
        const auto oldLayout = qgetenv("XKB_DEFAULT_LAYOUT"), oldOptions = qgetenv("XKB_DEFAULT_OPTIONS");
        qputenv("XKB_DEFAULT_LAYOUT", "de"); qputenv("XKB_DEFAULT_OPTIONS", "ctrl:nocaps");
        const auto result = KeyboardPreview::compile(request());
        if (oldLayout.isNull()) qunsetenv("XKB_DEFAULT_LAYOUT"); else qputenv("XKB_DEFAULT_LAYOUT", oldLayout);
        if (oldOptions.isNull()) qunsetenv("XKB_DEFAULT_OPTIONS"); else qputenv("XKB_DEFAULT_OPTIONS", oldOptions);
        QVERIFY2(result.valid(), qPrintable(result.error)); QCOMPARE(symbol(result, "AD06"), QStringLiteral("y"));
        QCOMPARE(symbol(result, "CAPS"), QStringLiteral("Caps Lock"));
    }
    void nativeDialogAllLevelsSelectionAndClose() {
        auto *dialog = new KeyboardPreviewDialog(request("de")); QPointer<KeyboardPreviewDialog> guard(dialog);
        QSignalSpy ready(dialog, &KeyboardPreviewDialog::previewReady); dialog->show(); QTRY_COMPARE(ready.count(), 1);
        QVERIFY2(dialog->result().valid(), qPrintable(dialog->result().error));
        auto *keys = dialog->findChild<QTreeWidget *>("keyboardPreviewKeys"); QVERIFY(keys); QVERIFY(!keys->accessibleName().isEmpty());
        auto *levels = dialog->findChild<QComboBox *>("keyboardPreviewLevel"); QVERIFY(levels); QVERIFY(levels->count() >= 4); levels->setCurrentIndex(2);
        QVERIFY(!levels->accessibleName().isEmpty());
        for (int i = 0; i < keys->topLevelItemCount(); ++i) if (keys->topLevelItem(i)->text(0) == "AD01") keys->setCurrentItem(keys->topLevelItem(i));
        QVERIFY(dialog->findChild<QLabel *>("keyboardPreviewKeyDetails")->text().contains("AD01"));
        const QString captures = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR");
        if (!captures.isEmpty()) { QDir().mkpath(captures); QVERIFY(dialog->grab().save(captures + QStringLiteral("/keyboard-preview.png"))); }
        auto *close = dialog->findChild<QPushButton *>("keyboardPreviewClose"); QVERIFY(close); QVERIFY(!close->accessibleName().isEmpty()); close->click(); QTRY_VERIFY(guard.isNull());
    }
    void destroyingDialogDuringCompilationIsSafe() {
        auto *dialog = new KeyboardPreviewDialog(request("us", "colemak")); delete dialog;
        // The worker owns its copied request and never dereferences the dialog.
        QTest::qWait(100); QApplication::processEvents();
    }
};
QTEST_MAIN(KeyboardPreviewTest)
#include "keyboard_preview_test.moc"
