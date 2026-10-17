#include <QtTest>
#include <QLabel>
#include <QListWidget>
#include <QScrollBar>
#include <QSignalSpy>

#include "ActionCatalog.h"
#include "Branding.h"
#include "CatalogView.h"

namespace {
QListWidget *items(CatalogView &view)
{
    return view.findChild<QListWidget *>(QStringLiteral("catalogItems"));
}

QList<int> ids(QListWidget *list)
{
    QList<int> result;
    for (int row = 0; row < list->count(); ++row)
        result.append(list->item(row)->data(Qt::UserRole).toInt());
    return result;
}

int rowFor(QListWidget *list, ActionId id)
{
    return ids(list).indexOf(static_cast<int>(id));
}
} // namespace

class CatalogViewTest : public QObject {
    Q_OBJECT

private slots:
    void iconModesHaveTheSameSortedUniqueItems()
    {
        CatalogView large(CatalogView::Mode::LargeIcons);
        CatalogView small(CatalogView::Mode::SmallIcons);
        auto *largeItems = items(large);
        auto *smallItems = items(small);
        QVERIFY(largeItems);
        QVERIFY(smallItems);
        QCOMPARE(largeItems->iconSize(), QSize(32, 32));
        QCOMPARE(smallItems->iconSize(), QSize(16, 16));
        QCOMPARE(ids(largeItems), ids(smallItems));
        QVERIFY(largeItems->count() > 0);
        QSet<int> seen;
        QString previous;
        for (int row = 0; row < largeItems->count(); ++row) {
            auto *item = largeItems->item(row);
            const int id = item->data(Qt::UserRole).toInt();
            QVERIFY(!seen.contains(id));
            seen.insert(id);
            const auto &action = ActionCatalog::get(static_cast<ActionId>(id));
            QVERIFY(action.iconItem);
            QCOMPARE(item->text(), Branding::brand(action.title));
            if (row > 0)
                QVERIFY(QString::localeAwareCompare(previous, item->text()) <= 0);
            previous = item->text();
            QVERIFY(!item->data(Qt::AccessibleTextRole).toString().isEmpty());
            QVERIFY(!item->data(Qt::AccessibleDescriptionRole).toString().isEmpty());
        }
        int expected = 0;
        for (const auto &action : ActionCatalog::all())
            expected += action.iconItem ? 1 : 0;
        QCOMPARE(largeItems->count(), expected);
    }

    void enabledKeyboardActivationEmitsOnlyStableIdentity()
    {
        CatalogView view(CatalogView::Mode::LargeIcons);
        view.resize(700, 400);
        view.show();
        auto *list = items(view);
        const int systemRow = rowFor(list, ActionId::System);
        QVERIFY(systemRow >= 0);
        QVERIFY(ActionCatalog::availability(ActionId::System).available);
        list->setCurrentRow(systemRow);
        list->setFocus();
        QList<ActionId> activated;
        connect(&view, &CatalogView::actionActivated, this,
            [&activated](ActionId id) { activated.append(id); });
        QTest::keyClick(list, Qt::Key_Return);
        QTest::keyClick(list, Qt::Key_Space);
        QTest::keyClick(list, Qt::Key_Enter);
        QCOMPARE(activated.size(), 3);
        for (ActionId id : activated)
            QCOMPARE(id, ActionId::System);
        for (Qt::KeyboardModifier modifier : {Qt::ControlModifier, Qt::AltModifier, Qt::ShiftModifier}) {
            QTest::keyClick(list, Qt::Key_Return, modifier);
            QTest::keyClick(list, Qt::Key_Enter, modifier);
            QTest::keyClick(list, Qt::Key_Space, modifier);
        }
        QCOMPARE(activated.size(), 3);
        // Arrow navigation remains the native list behavior.
        const int before = list->currentRow();
        QTest::keyClick(list, Qt::Key_Home);
        QTest::keyClick(list, Qt::Key_End);
        QVERIFY(list->currentRow() >= 0);
        QVERIFY(list->currentRow() != before || list->count() == 1);
        QCOMPARE(activated.size(), 3);
    }

    void unavailableResultsExplainWhyAndCannotActivate()
    {
        const auto availability = ActionCatalog::availability(ActionId::BackUpYourComputer);
        QVERIFY(!availability.available);
        CatalogView view(CatalogView::Mode::Search, QStringLiteral("back up"));
        view.resize(700, 400);
        view.show();
        auto *list = items(view);
        const int row = rowFor(list, ActionId::BackUpYourComputer);
        QVERIFY(row >= 0);
        auto *item = list->item(row);
        QVERIFY(!item->flags().testFlag(Qt::ItemIsEnabled));
        QVERIFY(item->toolTip().contains(Branding::brand(availability.reason).toHtmlEscaped()));
        QVERIFY(item->data(Qt::AccessibleDescriptionRole).toString().contains(Branding::brand(availability.reason)));
        // The reason is also a visible delegate field, not tooltip-only text.
        QVERIFY(item->data(Qt::UserRole + 3).toString().contains(Branding::brand(availability.reason)));
        QVERIFY(item->data(Qt::UserRole + 1).toString().isEmpty());
        QCOMPARE(item->data(Qt::AccessibleDescriptionRole).toString().count(Branding::brand(availability.reason)), 1);
        QSignalSpy activation(&view, &CatalogView::actionActivated);
        list->setCurrentRow(row);
        QTest::keyClick(list, Qt::Key_Return);
        QTest::keyClick(list, Qt::Key_Space);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          list->visualItemRect(item).center());
        QCOMPARE(activation.count(), 0);
    }

    void aliasAndEmptySearchMatchTheCatalog()
    {
        for (const QString &query : {QStringLiteral("Windows Firewall"), QString(), QStringLiteral("  ")}) {
            CatalogView view(CatalogView::Mode::Search, query);
            QList<int> expected;
            for (ActionId id : ActionCatalog::search(query))
                expected.append(static_cast<int>(id));
            QCOMPARE(ids(items(view)), expected);
            auto *count = view.findChild<QLabel *>(QStringLiteral("catalogResultCount"));
            QVERIFY(count);
            QVERIFY(count->text().contains(QString::number(expected.size())));
            if (query == QStringLiteral("Windows Firewall"))
                QVERIFY(ids(items(view)).contains(static_cast<int>(ActionId::LinuxFirewall)));
        }
    }

    void noMatchQueryIsPlainText()
    {
        const QString query = QStringLiteral("<b>zz_unmatched_catalog_query_803</b>");
        CatalogView view(CatalogView::Mode::Search, query);
        QCOMPARE(items(view)->count(), 0);
        auto *message = view.findChild<QLabel *>(QStringLiteral("catalogNoMatches"));
        QVERIFY(message);
        QCOMPARE(message->textFormat(), Qt::PlainText);
        QVERIFY(message->text().contains(query));
        QVERIFY(message->wordWrap());
    }

    void listsReflowAndScrollAtNarrowWidths()
    {
        for (CatalogView::Mode mode : {CatalogView::Mode::LargeIcons,
                                      CatalogView::Mode::SmallIcons, CatalogView::Mode::Search}) {
            CatalogView view(mode, mode == CatalogView::Mode::Search ? QStringLiteral("system") : QString());
            view.resize(700, 240);
            view.show();
            QCoreApplication::processEvents();
            auto *list = items(view);
            QVERIFY(list->count() > 0);
            const int originalCount = list->count();
            view.resize(320, 180);
            QCoreApplication::processEvents();
            QCOMPARE(list->count(), originalCount);
            QCOMPARE(list->horizontalScrollBar()->maximum(), 0);
            QVERIFY(list->verticalScrollBar()->maximum() > 0);
            for (int row = 0; row < list->count(); ++row) {
                const QRect rect = list->visualItemRect(list->item(row));
                QVERIFY(rect.width() <= list->viewport()->width());
                QVERIFY(rect.height() > 0);
            }
            list->scrollToBottom();
            QVERIFY(list->visualItemRect(list->item(list->count() - 1)).intersects(list->viewport()->rect()));
        }
    }

    void iconRowsUseAllAvailableColumns()
    {
        for (CatalogView::Mode mode : {CatalogView::Mode::LargeIcons, CatalogView::Mode::SmallIcons}) {
            CatalogView view(mode);
            view.resize(1000, 400);
            view.show();
            QCoreApplication::processEvents();
            auto *list = items(view);
            const int columns = list->viewport()->width() / 280;
            QCOMPARE(columns, 3);
            QVERIFY(list->count() > columns);
            const QRect first = list->visualItemRect(list->item(0));
            for (int column = 1; column < columns; ++column) {
                const QRect rect = list->visualItemRect(list->item(column));
                QCOMPARE(rect.top(), first.top());
                QVERIFY(rect.left() > first.left());
                QVERIFY(rect.right() < list->viewport()->width());
            }
            QVERIFY(list->visualItemRect(list->item(columns)).top() > first.top());
        }
    }
};

QTEST_MAIN(CatalogViewTest)
#include "catalog_view_test.moc"
