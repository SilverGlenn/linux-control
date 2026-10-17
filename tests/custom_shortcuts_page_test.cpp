#include <QtTest>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <AeroQt/stylesheet.h>
#include "settings/CustomShortcutsSettings.h"
#include "ui/pages/CustomShortcutsPage.h"

namespace {
CustomShortcutsSnapshot fixture() {
    CustomShortcutsSnapshot state; state.available = state.writable = true;
    state.owner = QStringLiteral(":1.42"); state.interfaceSignature = QStringLiteral("fixture-v1");
    CustomShortcutApplication alpha; alpha.id = "alpha.desktop"; alpha.name = "Alpha Editor"; alpha.exec = "alpha";
    CustomShortcutApplication bravo; bravo.id = "bravo.desktop"; bravo.name = "Bravo Browser"; bravo.exec = "bravo";
    state.applications = {alpha, bravo};
    CustomShortcutEntry app; app.component = alpha.id; app.name = alpha.name; app.exec = alpha.exec; app.registered = app.removable = true;
    CustomShortcutEntry command; command.component = "net.local.report.desktop"; command.name = "Report"; command.exec = "printf report";
    command.command = command.registered = command.editable = command.removable = true;
    CustomShortcutEntry protectedCommand = command; protectedCommand.component = "protected.desktop"; protectedCommand.name = "Protected";
    protectedCommand.editable = protectedCommand.removable = false; protectedCommand.reason = "Protected command definition";
    state.entries = {app, command, protectedCommand}; state.revision = CustomShortcutsTypes::revision(state); return state;
}
QTreeWidgetItem *item(QTreeWidget *tree, const QString &component) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == component) return tree->topLevelItem(i);
    return nullptr;
}
void select(CustomShortcutsPage &page, const QString &component) {
    auto *tree = page.findChild<QTreeWidget *>("customShortcutsEntries"); auto *row = item(tree, component); QVERIFY(row); tree->setCurrentItem(row);
}
void capture(CustomShortcutsPage &page) {
    const auto path = qEnvironmentVariable("LINUX_CONTROL_UI_CAPTURE_DIR"); if (path.isEmpty()) return;
    QDir().mkpath(path); page.resize(1100, 930); page.show(); QTest::qWait(30);
    QVERIFY(page.grab().save(path + QStringLiteral("/custom-shortcuts.png")));
}
}
class PageCustomBackend final : public CustomShortcutsBackend {
public:
    CustomShortcutsSnapshot current = fixture();
    int reads = 0, writes = 0; quint64 pending = 0;
    void readCurrent(quint64 token) override { ++reads; emit readFinished(token, true, current, {}); }
    void writeChanges(quint64 token, const CustomShortcutsSnapshot &, const CustomShortcutChanges &) override { ++writes; pending = token; }
    void fail() { emit writeFinished(pending, ShortcutsOutcome::Failed, current, QStringLiteral("Native operation failed")); }
};
class CustomShortcutsPageTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { if (qEnvironmentVariableIsSet("LINUX_CONTROL_UI_CAPTURE_DIR")) Aero::registerStylesheet(qApp); }
    void init() { backend = new PageCustomBackend; settings = new CustomShortcutsSettings(backend); QTRY_VERIFY(settings->isReady()); }
    void cleanup() { delete settings; }
    void installedApplicationSearchStagesRegistrationWithoutLaunching() {
        CustomShortcutsPage page(nullptr, settings);
        auto *filter = page.findChild<QLineEdit *>("customShortcutsApplicationFilter");
        auto *apps = page.findChild<QComboBox *>("customShortcutsApplications");
        auto *add = page.findChild<QPushButton *>("customShortcutsAddApplication");
        QVERIFY(!add->isEnabled()); filter->setText("bravo"); QCOMPARE(apps->count(), 1); QVERIFY(add->isEnabled());
        add->click(); QVERIFY(settings->isDirty()); QVERIFY(settings->staged().find("bravo.desktop")->pendingAddition); QCOMPARE(backend->writes, 0);
        select(page, "bravo.desktop"); QVERIFY(!page.findChild<QPushButton *>("customShortcutsAssign")->isEnabled());
        page.findChild<QPushButton *>("customShortcutsRemove")->click(); QVERIFY(!settings->staged().find("bravo.desktop")); QVERIFY(!settings->isDirty());
        QCOMPARE(backend->writes, 0);
    }
    void commandDefinitionIsStagedAndRevertedWithNativePercentEscaping() {
        CustomShortcutsPage page(nullptr, settings);
        auto *name = page.findChild<QLineEdit *>("customShortcutsName"); auto *command = page.findChild<QLineEdit *>("customShortcutsCommand");
        name->setText("Daily report"); command->setText("printf '100%'");
        page.findChild<QPushButton *>("customShortcutsSaveCommand")->click(); QVERIFY(settings->isDirty()); QCOMPARE(backend->writes, 0);
        const auto &created = settings->staged().entries.last(); QVERIFY(created.pendingAddition); QVERIFY(created.exec.contains("%%"));
        QVERIFY(name->text().isEmpty()); QVERIFY(command->text().isEmpty());
        select(page, created.component); QVERIFY(page.findChild<QLabel *>("customShortcutsDetails")->text().contains("Apply")); capture(page);
        page.findChild<QPushButton *>("customShortcutsRevert")->click(); QVERIFY(!settings->isDirty()); QCOMPARE(settings->staged().entries.size(), 3);
        QCOMPARE(backend->writes, 0);
    }
    void editRemovalAndKeepAreVisibleAndProtectedDefinitionsStayReadonly() {
        CustomShortcutsPage page(nullptr, settings); select(page, "net.local.report.desktop");
        page.findChild<QPushButton *>("customShortcutsEdit")->click();
        QCOMPARE(page.findChild<QLineEdit *>("customShortcutsName")->text(), "Report");
        page.findChild<QLineEdit *>("customShortcutsName")->setText("New report");
        page.findChild<QLineEdit *>("customShortcutsCommand")->setText("printf changed");
        page.findChild<QPushButton *>("customShortcutsSaveCommand")->click();
        QCOMPARE(settings->staged().find("net.local.report.desktop")->name, "New report");
        QCOMPARE(settings->applied().find("net.local.report.desktop")->name, "Report");
        select(page, "net.local.report.desktop"); page.findChild<QPushButton *>("customShortcutsRemove")->click();
        QVERIFY(settings->staged().find("net.local.report.desktop")->pendingRemoval);
        page.findChild<QPushButton *>("customShortcutsRestore")->click(); QVERIFY(!settings->staged().find("net.local.report.desktop")->pendingRemoval);
        select(page, "protected.desktop"); QVERIFY(!page.findChild<QPushButton *>("customShortcutsEdit")->isEnabled());
        QVERIFY(!page.findChild<QPushButton *>("customShortcutsRemove")->isEnabled());
        QVERIFY(page.findChild<QLabel *>("customShortcutsDetails")->text().contains("Protected")); QCOMPARE(backend->writes, 0);
    }
    void backgroundApplySurvivesPageDestructionAndFailureKeepsDraft() {
        auto *page = new CustomShortcutsPage(nullptr, settings);
        page->findChild<QLineEdit *>("customShortcutsName")->setText("Report"); page->findChild<QLineEdit *>("customShortcutsCommand")->setText("printf report");
        page->findChild<QPushButton *>("customShortcutsSaveCommand")->click();
        page->findChild<QPushButton *>("customShortcutsApply")->click(); QCOMPARE(backend->writes, 1); QVERIFY(settings->isBusy()); delete page;
        QVERIFY(settings->isBusy()); page = new CustomShortcutsPage(nullptr, settings);
        QVERIFY(!page->findChild<QPushButton *>("customShortcutsApply")->isEnabled()); backend->fail();
        QVERIFY(!settings->isBusy()); QVERIFY(settings->isDirty()); QVERIFY(page->findChild<QLabel *>("customShortcutsStatus")->text().contains("Native operation failed"));
        delete page;
    }
    void savedUnregisteredCommandCanBeRegisteredWithoutCreatingAnotherFile() {
        CustomShortcutApplication app; app.id = "net.local.saved.desktop"; app.name = "Saved command"; app.exec = "printf saved"; app.command = true;
        backend->current.applications << app;
        CustomShortcutEntry entry; entry.component = app.id; entry.name = app.name; entry.exec = app.exec;
        entry.command = entry.editable = entry.removable = true; backend->current.entries << entry;
        backend->current.revision = CustomShortcutsTypes::revision(backend->current); emit backend->externalChange();
        QTRY_VERIFY(settings->staged().find(app.id)); CustomShortcutsPage page(nullptr, settings); select(page, app.id);
        auto *registerButton = page.findChild<QPushButton *>("customShortcutsRegister"); QVERIFY(registerButton->isEnabled()); registerButton->click();
        QVERIFY(settings->staged().find(app.id)->pendingAddition); QCOMPARE(backend->writes, 0);
        QVERIFY(!page.findChild<QPushButton *>("customShortcutsAssign")->isEnabled());
    }
private:
    PageCustomBackend *backend = nullptr; CustomShortcutsSettings *settings = nullptr;
};
QTEST_MAIN(CustomShortcutsPageTest)
#include "custom_shortcuts_page_test.moc"
