#include <QtTest>
#include <QFile>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <KConfig>
#include <KConfigGroup>
#include "platform/kde/ThemeComponentBackend.h"
#include <functional>

class FakeComponentReload : public ThemeComponentReloadService {
public:
    std::array<bool, 3> available = {true, true, true};
    bool success = true;
    QList<int> requests;
    std::function<void()> onRequest;
    bool isAvailable(ThemeComponent component) const override { return available[static_cast<int>(component)]; }
    QString unavailableReason(ThemeComponent) const override { return "Reload unavailable"; }
    bool requestReload(ThemeComponent component, QString &error) override
    {
        requests << static_cast<int>(component);
        if (onRequest) onRequest();
        if (!success) error = "Saved; reload unavailable";
        return success;
    }
};

class ThemeComponentBackendTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<ThemeComponentState>(); }
    void init()
    {
        QVERIFY(temp.isValid());
        QFile::remove(path("kdeglobals")); QFile::remove(path("kcminputrc"));
        reload = new FakeComponentReload;
        ThemeComponentChoices choices;
        choices[0] = {{"breeze", "Breeze"}, {"Windows 7 Aero", "Windows 7 Aero"}};
        choices[1] = {{"default", "Default"}, {"aero-drop", "Aero cursors"}};
        choices[2] = {{"Breeze", "Breeze"}, {"kvantum", "kvantum"}};
        backend = new KdeThemeComponentBackend(temp.path(), reload, choices);
    }
    void cleanup() { delete backend; }
    void defaultsReadWithoutCreatingFilesOrReloads()
    {
        const auto state = read();
        QCOMPARE(state.values(), original);
        QVERIFY(state.fields[0].canApply());
        QCOMPARE(state.fields[0].choices.size(), 1);
        QCOMPARE(state.fields[0].choices[0].id, "Windows 7 Aero");
        QCOMPARE(state.fields[1].choices[0].id, "aero-drop");
        QCOMPARE(state.fields[2].choices[0].id, "kvantum");
        QVERIFY(!QFile::exists(path("kdeglobals")));
        QVERIFY(!QFile::exists(path("kcminputrc")));
        QVERIFY(reload->requests.isEmpty());
    }
    void allComponentsWriteOnlyTheirOwnKeyAndReadback()
    {
        put("kdeglobals", "[Icons]\nTheme=breeze\nSize=48\n[KDE]\nwidgetStyle=Breeze\nShowIconsInMenuItems=false\n[General]\nColorScheme=KeepMe\n");
        put("kcminputrc", "[Mouse]\ncursorTheme=default\ncursorSize=36\n[Other]\nValue=keep\n");
        QSignalSpy finished(backend, &ThemeComponentBackend::writeFinished);
        backend->writeComponents(original, selected);
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished[0][0].toBool());
        QCOMPARE(read().values(), selected);
        QCOMPARE(reload->requests, (QList<int>{0, 1, 2}));
        KConfig globals(path("kdeglobals"), KConfig::SimpleConfig);
        QCOMPARE(KConfigGroup(&globals, "Icons").readEntry("Size", 0), 48);
        QCOMPARE(KConfigGroup(&globals, "General").readEntry("ColorScheme"), "KeepMe");
        QCOMPARE(KConfigGroup(&globals, "KDE").readEntry("ShowIconsInMenuItems", true), false);
        KConfig input(path("kcminputrc"), KConfig::SimpleConfig);
        QCOMPARE(KConfigGroup(&input, "Mouse").readEntry("cursorSize", 0), 36);
        QCOMPARE(KConfigGroup(&input, "Other").readEntry("Value"), "keep");
    }
    void immutableSiblingRejectsAllChangedFieldsBeforeFirstWrite()
    {
        put("kcminputrc", "[Mouse]\ncursorTheme[$i]=default\ncursorSize=24\n");
        auto state = read();
        QVERIFY(!state.fields[1].writable);
        QVERIFY(state.fields[0].canApply());
        QSignalSpy finished(backend, &ThemeComponentBackend::writeFinished);
        backend->writeComponents(original, selected);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(!QFile::exists(path("kdeglobals")));
        QVERIFY(reload->requests.isEmpty());
        // An unchanged locked component does not block other components.
        backend->writeComponents(original, {"Windows 7 Aero", "default", "Breeze"});
        QVERIFY(finished[1][0].toBool());
        QCOMPARE(read().values(), (QStringList{"Windows 7 Aero", "default", "Breeze"}));
    }
    void unavailableReloadAndUnknownThemesNeverWrite()
    {
        reload->available[2] = false;
        QVERIFY(!read().fields[2].canApply());
        backend->writeComponents(original, selected);
        QVERIFY(!QFile::exists(path("kdeglobals")));
        reload->available[2] = true;
        backend->writeComponents(original, {"../../arbitrary", "default", "Breeze"});
        QVERIFY(!QFile::exists(path("kdeglobals")));
        QVERIFY(reload->requests.isEmpty());
    }
    void installedUnapprovedChoicesNeverWrite()
    {
        delete backend;
        reload = new FakeComponentReload;
        ThemeComponentChoices choices;
        choices[0] = {{"oxygen", "Installed Oxygen"}};
        choices[1] = {{"Breeze_Snow", "Installed Breeze Snow"}};
        choices[2] = {{"Fusion", "Installed Fusion"}, {"Kvantum", "Case variant"}};
        backend = new KdeThemeComponentBackend(temp.path(), reload, choices);
        QSignalSpy finished(backend, &ThemeComponentBackend::writeFinished);
        for (const auto &requested : {QStringList{"oxygen", "default", "Breeze"},
                                      QStringList{"breeze", "Breeze_Snow", "Breeze"},
                                      QStringList{"breeze", "default", "Fusion"},
                                      QStringList{"breeze", "default", "Kvantum"}}) {
            backend->writeComponents(original, requested);
            QVERIFY(!finished.last()[0].toBool());
        }
        QVERIFY(!QFile::exists(path("kdeglobals")));
        QVERIFY(!QFile::exists(path("kcminputrc")));
        QVERIFY(reload->requests.isEmpty());
    }
    void productionLegacyWritesAreRetiredEvenForApprovedNames()
    {
        KdeThemeComponentBackend production;
        QSignalSpy finished(&production, &ThemeComponentBackend::writeFinished);
        production.writeComponents(original, selected);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(finished[0][1].toString().contains("Personalization"));
    }
    void entireBaselineIsCheckedImmediatelyBeforeWrite()
    {
        put("kcminputrc", "[Mouse]\ncursorTheme=aero-drop\n");
        backend->writeComponents(original, {"Windows 7 Aero", "default", "Breeze"});
        QVERIFY(!QFile::exists(path("kdeglobals")));
        QVERIFY(reload->requests.isEmpty());
    }
    void midApplyExternalEditAbortsRemainingWritesWithoutRollback()
    {
        reload->onRequest = [this] { put("kcminputrc", "[Mouse]\ncursorTheme=ExternalChoice\n"); };
        QSignalSpy finished(backend, &ThemeComponentBackend::writeFinished);
        backend->writeComponents(original, selected);
        QVERIFY(!finished[0][0].toBool());
        QCOMPARE(reload->requests, (QList<int>{0}));
        QCOMPARE(read().values(), (QStringList{"Windows 7 Aero", "ExternalChoice", "Breeze"}));
    }
    void reloadFailureRetainsAuthoritativeSavedValue()
    {
        reload->success = false;
        QSignalSpy finished(backend, &ThemeComponentBackend::writeFinished);
        backend->writeComponents(original, selected);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(finished[0][1].toString().contains("Saved"));
        QCOMPARE(read().values(), (QStringList{"Windows 7 Aero", "default", "Breeze"}));
        QCOMPARE(reload->requests, (QList<int>{0}));
    }
    void atomicConfigReplacementRemainsWatched()
    {
        QSignalSpy changed(backend, &ThemeComponentBackend::externalChange);
        atomicPut("kcminputrc", "[Mouse]\ncursorTheme=aero-drop\n");
        QTRY_VERIFY(!changed.isEmpty());
        changed.clear();
        atomicPut("kcminputrc", "[Mouse]\ncursorTheme=default\n");
        QTRY_VERIFY(!changed.isEmpty());
        QCOMPARE(read().values(), original);
        QVERIFY(reload->requests.isEmpty());
    }
private:
    QString path(const QString &file) const { return temp.filePath(file); }
    void put(const QString &name, const QByteArray &text)
    {
        QFile file(path(name));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(text), qint64(text.size()));
        file.close();
    }
    void atomicPut(const QString &name, const QByteArray &text)
    {
        QSaveFile file(path(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(text), qint64(text.size()));
        QVERIFY(file.commit());
    }
    ThemeComponentState read()
    {
        QSignalSpy result(backend, &ThemeComponentBackend::readFinished);
        backend->readCurrent();
        if (result.size() != 1 || !result[0][0].toBool()) {
            QTest::qFail("Temporary configuration read failed", __FILE__, __LINE__);
            return {};
        }
        return qvariant_cast<ThemeComponentState>(result[0][1]);
    }
    QTemporaryDir temp;
    FakeComponentReload *reload = nullptr;
    KdeThemeComponentBackend *backend = nullptr;
    const QStringList original{"breeze", "default", "Breeze"};
    const QStringList selected{"Windows 7 Aero", "aero-drop", "kvantum"};
};
QTEST_GUILESS_MAIN(ThemeComponentBackendTest)
#include "theme_component_backend_test.moc"
