#include <QtTest>
#include <QAccessible>
#include <QEnterEvent>
#include <QSignalSpy>

#include "ActionCatalog.h"
#include "CategoryWidget.h"
#include "LinkLabel.h"
#include "Win7Ui.h"

class UiControlsTest : public QObject {
    Q_OBJECT

private slots:
    void linkActivationRespectsAvailability()
    {
        LinkLabel link(QStringLiteral("A setting"));
        link.resize(180, 30);
        link.show();
        QSignalSpy clicked(&link, &LinkLabel::clicked);
        QTest::mouseClick(&link, Qt::LeftButton);
        QCOMPARE(clicked.count(), 1);

        QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
        QApplication::sendEvent(&link, &enter);
        QVERIFY(link.font().underline());

        link.setEnabled(false);
        QVERIFY(!link.font().underline());
        QCOMPARE(link.cursor().shape(), Qt::ArrowCursor);
        QCOMPARE(link.focusPolicy(), Qt::NoFocus);
        QTest::mouseClick(&link, Qt::LeftButton);
        QTest::keyClick(&link, Qt::Key_Return);
        QTest::keyClick(&link, Qt::Key_Space);
        QApplication::sendEvent(&link, &enter);
        QCOMPARE(clicked.count(), 1);
        QVERIFY(!link.font().underline());

        link.setEnabled(true);
        QCOMPARE(link.cursor().shape(), Qt::PointingHandCursor);
        QCOMPARE(link.focusPolicy(), Qt::StrongFocus);
        QTest::mouseClick(&link, Qt::LeftButton);
        QCOMPARE(clicked.count(), 2);
    }

    void linkNeedsMatchingPressAndRelease()
    {
        LinkLabel link(QStringLiteral("A setting"));
        link.resize(180, 30);
        link.show();
        QSignalSpy clicked(&link, &LinkLabel::clicked);
        QTest::mouseRelease(&link, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
        QTest::mouseClick(&link, Qt::RightButton);
        QTest::mousePress(&link, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
        QTest::mouseRelease(&link, Qt::LeftButton, Qt::NoModifier, QPoint(200, 8));
        QCOMPARE(clicked.count(), 0);
        QTest::mouseRelease(&link, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
        QCOMPARE(clicked.count(), 0);
        QTest::mousePress(&link, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
        link.setEnabled(false);
        link.setEnabled(true);
        QTest::mouseRelease(&link, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
        QCOMPARE(clicked.count(), 0);
    }

    void linkKeyboardAndFocusCue()
    {
        LinkLabel link(QStringLiteral("A setting"));
        link.setStyleSheet("QLabel { background: white; color: #1F4E99; border: none; }");
        link.resize(180, 30);
        link.show();
        link.activateWindow();
        link.setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(link.hasFocus());
        QSignalSpy clicked(&link, &LinkLabel::clicked);
        QTest::keyClick(&link, Qt::Key_Return);
        QTest::keyClick(&link, Qt::Key_Enter);
        QTest::keyClick(&link, Qt::Key_Space);
        QCOMPARE(clicked.count(), 3);
        QTest::keyClick(&link, Qt::Key_Return, Qt::ControlModifier);
        QTest::keyRelease(&link, Qt::Key_Space);
        QCOMPARE(clicked.count(), 3);

        const QImage focused = link.grab().toImage();
        link.clearFocus();
        const QImage unfocused = link.grab().toImage();
        QVERIFY(focused != unfocused);
        // The focus cue is a painted dotted border, even with a borderless stylesheet.
        bool borderChanged = false;
        for (int x = 1; x < link.width() - 1; ++x)
            borderChanged |= focused.pixel(x, 1) != unfocused.pixel(x, 1);
        QVERIFY(borderChanged);
    }

    void accessibleLinkCanActivateAndReportsDisabled()
    {
        LinkLabel link(QStringLiteral("Open a setting"));
        QSignalSpy clicked(&link, &LinkLabel::clicked);
        QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(&link);
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::Link);
        QCOMPARE(accessible->text(QAccessible::Name), link.text());
        QVERIFY(accessible->state().focusable);
        QAccessibleActionInterface *actions = accessible->actionInterface();
        QVERIFY(actions);
        QVERIFY(actions->actionNames().contains(QAccessibleActionInterface::pressAction()));
        actions->doAction(QAccessibleActionInterface::pressAction());
        QCOMPARE(clicked.count(), 1);
        link.setText(QStringLiteral("Updated setting"));
        QCOMPARE(accessible->text(QAccessible::Name), link.text());
        link.setEnabled(false);
        QVERIFY(accessible->state().disabled);
        QVERIFY(!accessible->state().focusable);
        QVERIFY(actions->actionNames().isEmpty());
        actions->doAction(QAccessibleActionInterface::pressAction());
        QCOMPARE(clicked.count(), 1);
    }

    void categoryUsesActionIdsAndCanonicalTitle()
    {
        const ActionId action = ActionId::System;
        const QString categoryName = QStringLiteral("System and Security");
        CategoryWidget category({QStringLiteral("preferences-system"), categoryName, {action}});
        category.show();
        QSignalSpy titleActivated(&category, &CategoryWidget::titleActivated);
        QSignalSpy actionActivated(&category, &CategoryWidget::actionActivated);
        const auto links = category.findChildren<LinkLabel *>();
        QCOMPARE(links.size(), 2);
        QTest::keyClick(links[0], Qt::Key_Return);
        QCOMPARE(titleActivated.count(), 1);
        QCOMPARE(titleActivated.takeFirst().at(0).toString(), categoryName);
        QTest::keyClick(links[1], Qt::Key_Space);
        QCOMPARE(actionActivated.count(), 1);
        QCOMPARE(qvariant_cast<ActionId>(actionActivated.takeFirst().at(0)), action);
    }

    void unavailableHomeTaskCannotActivate()
    {
        const ActionId action = ActionId::BackUpYourComputer;
        CategoryWidget category({QStringLiteral("preferences-system"),
                                 QStringLiteral("Category"), {action}});
        category.show();
        QSignalSpy activated(&category, &CategoryWidget::actionActivated);
        const auto links = category.findChildren<LinkLabel *>();
        QCOMPARE(links.size(), 2);
        LinkLabel *taskLabel = links[1];
        QVERIFY(!taskLabel->isEnabled());
        QVERIFY(!taskLabel->toolTip().isEmpty());
        QCOMPARE(taskLabel->focusPolicy(), Qt::NoFocus);
        QTest::mouseClick(taskLabel, Qt::LeftButton);
        QTest::keyClick(taskLabel, Qt::Key_Return);
        QTest::keyClick(taskLabel, Qt::Key_Space);
        QCOMPARE(activated.count(), 0);
        category.setTaskAvailable(action, true);
        QCOMPARE(taskLabel->focusPolicy(), Qt::StrongFocus);
        QTest::keyClick(taskLabel, Qt::Key_Return);
        QCOMPARE(activated.count(), 1);
        category.setTaskAvailable(action, false, QStringLiteral("Unavailable during refresh"));
        QCOMPARE(taskLabel->toolTip(), QStringLiteral("Unavailable during refresh"));
        QCOMPARE(taskLabel->accessibleDescription(), taskLabel->toolTip());
        QTest::keyClick(taskLabel, Qt::Key_Return);
        QCOMPARE(activated.count(), 1);
    }

    void tabSkipsUnavailableLinks()
    {
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *first = new LinkLabel(QStringLiteral("First"));
        auto *disabled = new LinkLabel(QStringLiteral("Unavailable"));
        auto *last = new LinkLabel(QStringLiteral("Last"));
        layout->addWidget(first);
        layout->addWidget(disabled);
        layout->addWidget(last);
        disabled->setEnabled(false);
        window.show();
        window.activateWindow();
        first->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(first->hasFocus());
        QTest::keyClick(first, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), last);
    }

    void sectionHeaderAndChevronSupportKeyboard()
    {
        Win7::ClickableWidget header;
        header.setAccessibleName(QStringLiteral("Security"));
        header.resize(200, 36);
        header.show();
        int activations = 0;
        header.onClick = [&activations] { ++activations; };
        QTest::keyClick(&header, Qt::Key_Space);
        QVERIFY(header.isChecked());
        QTest::keyClick(&header, Qt::Key_Return);
        QCOMPARE(activations, 2);
        QVERIFY(!header.isChecked());
        header.setEnabled(false);
        QTest::keyClick(&header, Qt::Key_Return);
        QCOMPARE(activations, 2);

        Win7::ChevronButton chevron;
        QCOMPARE(chevron.focusPolicy(), Qt::StrongFocus);
        QTest::keyClick(&chevron, Qt::Key_Return);
        QVERIFY(chevron.isChecked());
        QTest::keyClick(&chevron, Qt::Key_Space);
        QVERIFY(!chevron.isChecked());
        Win7::ChevronButton decoration(false);
        QCOMPARE(decoration.focusPolicy(), Qt::NoFocus);
        QVERIFY(!decoration.isEnabled());
        decoration.setChecked(true);
        QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(&decoration);
        QVERIFY(accessible);
        QVERIFY(accessible->state().disabled);
        QAccessibleActionInterface *actions = accessible->actionInterface();
        QVERIFY(actions);
        actions->doAction(QAccessibleActionInterface::pressAction());
        actions->doAction(QAccessibleActionInterface::toggleAction());
        QVERIFY(decoration.isChecked());
        QTest::keyClick(&decoration, Qt::Key_Return);
        QTest::keyClick(&decoration, Qt::Key_Space);
        QVERIFY(decoration.isChecked());
    }
};

QTEST_MAIN(UiControlsTest)
#include "ui_controls_test.moc"
