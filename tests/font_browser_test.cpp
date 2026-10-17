#include "ui/pages/FontsPage.h"
#include "settings/UserFontService.h"
#include "operations/OperationManager.h"
#include <QTest>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QTextEdit>

namespace {
void script(const QString &path, const QByteArray &text)
{
    QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("#!/bin/sh\n" + text); f.close();
    QVERIFY(f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
}
}
class FontBrowserTest : public QObject {
    Q_OBJECT
private slots:
    void nativeSearchKeyboardAndPreview();
    void inventoryOutlivesPage();
};
void FontBrowserTest::nativeSearchKeyboardAndPreview()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    UserFontService::Options opts;
    opts.dataDirectory = dir.path() + "/data";
    opts.listProgram = dir.path() + "/list";
    script(opts.listProgram, "printf 'Alpha\\t/tmp/alpha.ttf\\nBeta\\t/tmp/beta.otf\\nGamma\\t/tmp/gamma.ttf\\n'\n");
    UserFontService service(opts);
    service.refresh(); QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 3000);
    FontsPage page(nullptr, &service); page.resize(1000, 700); page.show();
    auto tiles = page.findChildren<QWidget *>("fontTile"); QCOMPARE(tiles.size(), 3);
    auto *search = page.findChild<QLineEdit *>("fontSearch"); QVERIFY(search);
    search->setText("be");
    tiles = page.findChildren<QWidget *>("fontTile");
    int visible = 0; QWidget *beta = nullptr;
    for (auto *tile : tiles) if (!tile->isHidden()) { ++visible; beta = tile; }
    QCOMPARE(visible, 1); QVERIFY(beta); QCOMPARE(beta->accessibleName(), "Beta");
    beta->setFocus(); QTest::keyClick(beta, Qt::Key_Space);
    auto *preview = page.findChild<QDialog *>("fontPreviewDialog"); QVERIFY(preview); QVERIFY(preview->isVisible());
    auto *sample = preview->findChild<QTextEdit *>("fontPreviewSample"); QVERIFY(sample); QVERIFY(!sample->isReadOnly());
    preview->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    search->clear();
    tiles = page.findChildren<QWidget *>("fontTile");
    QWidget *alpha = nullptr;
    for (auto *tile : tiles) if (tile->accessibleName() == "Alpha") alpha = tile;
    QVERIFY(alpha); alpha->setFocus(); QTest::keyClick(alpha, Qt::Key_Right);
    QVERIFY(page.findChild<QLabel *>("fontSelectionDetails")->text().startsWith("Beta"));
    QVERIFY(!page.findChild<QPushButton *>("fontRemove")->isEnabled());
    QVERIFY(page.findChild<QLabel *>("fontSelectionDetails")->text().contains("Removal is unavailable"));
    auto *focused = page.focusWidget(); QVERIFY(focused); QTest::keyClick(focused, Qt::Key_Return);
    QVERIFY(page.findChild<QDialog *>("fontPreviewDialog"));
    page.findChild<QDialog *>("fontPreviewDialog")->close();
    page.findChild<QPushButton *>("fontRefresh")->click();
    QVERIFY(service.busy()); QVERIFY(!page.findChild<QPushButton *>("fontRefresh")->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 3000);
}
void FontBrowserTest::inventoryOutlivesPage()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    UserFontService::Options opts;
    opts.dataDirectory = dir.path() + "/data"; opts.listProgram = dir.path() + "/list";
    script(opts.listProgram, "sleep 0.15\nprintf 'Persistent\\t/tmp/font.ttf\\n'\n");
    UserFontService service(opts);
    auto *page = new FontsPage(nullptr, &service);
    QTRY_VERIFY(service.busy());
    QVERIFY(OperationManager::instance()->hasActiveJobs());
    delete page;
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 3000);
    QVERIFY(service.inventoryValid()); QCOMPARE(service.families().size(), 1);
    FontsPage returned(nullptr, &service);
    QCOMPARE(returned.findChildren<QWidget *>("fontTile").size(), 1);
    QVERIFY(!OperationManager::instance()->hasActiveJobs());
}
QTEST_MAIN(FontBrowserTest)
#include "font_browser_test.moc"
