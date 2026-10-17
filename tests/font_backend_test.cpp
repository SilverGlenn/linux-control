#include <QtTest>
#include <QFile>
#include <QDir>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDomDocument>
#include <KConfig>
#include <KConfigGroup>
#include "platform/kde/FontBackend.h"

class FakeFontReload : public FontReloadService {
public:
    bool available = true, success = true, complete = true;
    int requests = 0;
    bool isAvailable() const override { return available; }
    QString unavailableReason() const override { return QStringLiteral("No fake font reload service"); }
    void requestReload() override {
        ++requests;
        if (complete) emit finished(success, success ? QString() : QStringLiteral("Saved but refresh request failed"));
    }
};
class FontBackendTest : public QObject {
    Q_OBJECT
private slots:
    void includeDirectoriesAreDependenciesNotXmlFiles() {
        QTemporaryDir root; QVERIFY(root.isValid());
        const QString includeDir = root.filePath("conf.d"); QVERIFY(QDir().mkpath(includeDir));
        QByteArray empty, added, edited; QString error;
        QVERIFY2(fontConfigurationRevision({includeDir}, empty, error), qPrintable(error));
        const QString included = includeDir + "/50-local.conf";
        put(included, "<fontconfig/>");
        QVERIFY(fontConfigurationRevision({includeDir, included}, added, error)); QVERIFY(empty != added);
        put(included, "<fontconfig><!--external--></fontconfig>");
        QVERIFY(fontConfigurationRevision({includeDir, included}, edited, error)); QVERIFY(added != edited);
        QVERIFY(QFile::remove(included));
        QByteArray removed; QVERIFY(fontConfigurationRevision({includeDir}, removed, error)); QCOMPARE(removed, empty);
    }
    void initTestCase() { qRegisterMetaType<FontState>(); }
    void init() {
        QVERIFY(temp.isValid()); config = temp.filePath("kdeglobals"); xml = temp.filePath("fonts.conf");
        QFile::remove(config); QFile::remove(xml); reload = new FakeFontReload;
        backend = new KdeFontBackend(config, xml, reload);
    }
    void cleanup() { delete backend; }
    void failedSyncCannotRetryAfterFailureObserverRestoresDestination() {
        delete backend; backend = nullptr;
        const auto parent = temp.filePath("font-failure"), parked = temp.filePath("font-failure-parked");
        QVERIFY(QDir().mkpath(parent)); config = parent + "/kdeglobals"; xml = parent + "/fonts.conf";
        const QByteArray original = "[Unrelated]\nKeep=original\n"; put(config, original);
        reload = new FakeFontReload; backend = new KdeFontBackend(config, xml, reload);
        const auto baseline = read(); auto requested = baseline.preferences; requested.antialias = "true";
        requested.fonts[0].setPointSize(requested.fonts[0].pointSize() + 1);
        bool dirty = false, blocked = false, syncFailed = false, restored = false;
        backend->setSyncForTest([&](KConfig &config) {
            dirty = config.isDirty();
            if (!QDir().rename(parent, parked)) return false;
            QFile blocker(parent); blocked = blocker.open(QIODevice::WriteOnly); blocker.close();
            const bool result = config.sync(); syncFailed = !result; return result;
        });
        connect(backend, &FontBackend::writeFinished, this, [&](bool success, const QString &) {
            if (!success && blocked) restored = QFile::remove(parent) && QDir().rename(parked, parent);
        });
        QSignalSpy finished(backend, &FontBackend::writeFinished); backend->writePreferences(baseline, requested);
        QCOMPARE(finished.size(), 1); QVERIFY(!finished[0][0].toBool()); QVERIFY(finished[0][1].toString().contains("partial save"));
        QVERIFY(dirty && blocked && syncFailed && restored); QCOMPARE(contents(config), original);
        QVERIFY(contents(xml).contains("antialias")); QCOMPARE(reload->requests, 0);
        delete backend; backend = nullptr; QCoreApplication::processEvents();
        QCOMPARE(contents(config), original); QVERIFY(contents(xml).contains("antialias"));
    }
    void absentSettingsReadDoesNotCreateFiles() {
        const auto s = read(); QVERIFY(s.canApply()); QCOMPARE(s.preferences.fonts[0].family(), "Noto Sans");
        QCOMPARE(s.preferences.fonts[1].family(), "Hack"); QCOMPARE(s.preferences.fonts[2].pointSize(), 8);
        QVERIFY(s.preferences.antialias.isEmpty()); QVERIFY(!QFile::exists(config)); QVERIFY(!QFile::exists(xml)); QCOMPARE(reload->requests, 0);
    }
    void rolesAndRenderingUseKdeAndFontconfigSemantics() {
        put(config, "[General]\nColorScheme=BreezeDark\n[Unrelated]\nValue=keep\n");
        put(xml, "<?xml version=\"1.0\"?><fontconfig><!--keep--><alias><family>Foo</family><prefer><family>Bar</family></prefer></alias><match target=\"font\"><test name=\"size\" compare=\"less\"><double>8</double></test><edit name=\"antialias\" mode=\"assign\"><bool>false</bool></edit></match></fontconfig>");
        auto s = read(); auto p = s.preferences;
        p.fonts[0] = QFont("Noto Serif", 12); p.fonts[5] = QFont("Noto Serif", 13);
        p.antialias = "true"; p.hinting = "hintslight"; p.subpixel = "rgb";
        QSignalSpy finished(backend, &FontBackend::writeFinished); backend->writePreferences(s, p);
        QCOMPARE(finished.size(), 1); QVERIFY(finished[0][0].toBool()); QCOMPARE(reload->requests, 1);
        QCOMPARE(read().preferences, p);
        KConfig cfg(config, KConfig::SimpleConfig); KConfigGroup general(&cfg, "General");
        QCOMPARE(general.readEntry("ColorScheme"), "BreezeDark"); QCOMPARE(KConfigGroup(&cfg, "Unrelated").readEntry("Value"), "keep");
        QCOMPARE(general.readEntry("XftAntialias", false), true); QCOMPARE(general.readEntry("XftHintStyle"), "hintslight");
        QCOMPARE(general.readEntry("XftSubPixel"), "rgb"); QVERIFY(!general.hasKey("fixed"));
        const auto bytes = contents(xml); QVERIFY(bytes.contains("<!--keep-->")); QVERIFY(bytes.contains("<alias>"));
        QDomDocument doc; QVERIFY(doc.setContent(bytes));
        auto root = doc.documentElement(); QCOMPARE(root.elementsByTagName("test").count(), 1);
        const auto matches = root.elementsByTagName("match");
        // New general antialias rule precedes the existing size exclusion.
        QVERIFY(matches.count() >= 2);
        QCOMPARE(matches.at(0).toElement().firstChildElement("edit").attribute("name"), "antialias");
        QCOMPARE(matches.at(1).toElement().firstChildElement("test").attribute("name"), "size");
        QVERIFY(bytes.contains("hinting")); QVERIFY(bytes.contains("<bool>true</bool>"));
    }
    void fontOnlyChangeDoesNotMaterializeRenderingOverrides() {
        auto s = read(); auto p = s.preferences; p.fonts[1] = QFont("Monospace", 11);
        backend->writePreferences(s, p); QCOMPARE(reload->requests, 1); QVERIFY(!QFile::exists(xml));
        KConfig cfg(config, KConfig::SimpleConfig); KConfigGroup general(&cfg, "General");
        QVERIFY(!general.hasKey("XftAntialias")); QVERIFY(!general.hasKey("font")); QCOMPARE(read().preferences, p);
    }
    void vendorDefaultRemovesOnlyUnconditionalRenderingRules() {
        put(xml, "<fontconfig><match target=\"font\"><edit name=\"antialias\" mode=\"assign\"><bool>false</bool></edit></match><match target=\"font\"><test name=\"family\"><string>Special</string></test><edit name=\"antialias\" mode=\"assign\"><bool>false</bool></edit></match></fontconfig>");
        put(config, "[General]\nXftAntialias=false\nOther=keep\n");
        auto s = read(); QCOMPARE(s.preferences.antialias, "false"); auto p = s.preferences; p.antialias.clear();
        backend->writePreferences(s, p); QCOMPARE(reload->requests, 1); QCOMPARE(read().preferences.antialias, QString());
        QDomDocument doc; QVERIFY(doc.setContent(contents(xml))); QCOMPARE(doc.elementsByTagName("edit").count(), 1); QCOMPARE(doc.elementsByTagName("test").count(), 1);
        KConfig cfg(config, KConfig::SimpleConfig); QVERIFY(!KConfigGroup(&cfg, "General").hasKey("XftAntialias"));
    }
    void immutableRolesOrRenderingDisableAllWrites_data() {
        QTest::addColumn<QByteArray>("data");
        QTest::newRow("font") << QByteArray("[General]\nfont[$i]=Noto Sans,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1\n");
        QTest::newRow("rendering") << QByteArray("[General]\nXftHintStyle[$i]=hintslight\n");
        QTest::newRow("title") << QByteArray("[WM][$i]\nactiveFont=Noto Sans,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1\n");
    }
    void immutableRolesOrRenderingDisableAllWrites() {
        QFETCH(QByteArray, data); put(config, data); auto s = read(); QVERIFY(!s.writable);
        auto p = s.preferences; p.antialias = "true"; QSignalSpy finished(backend, &FontBackend::writeFinished);
        backend->writePreferences(s, p); QVERIFY(!finished[0][0].toBool()); QCOMPARE(contents(config), data); QVERIFY(!QFile::exists(xml)); QCOMPARE(reload->requests, 0);
    }
    void preflightDetectsConfigAndXmlExternalEdits() {
        auto s = read(); auto p = s.preferences; p.antialias = "false";
        put(xml, "<fontconfig><!--external--></fontconfig>");
        QSignalSpy finished(backend, &FontBackend::writeFinished); backend->writePreferences(s, p);
        QVERIFY(!finished[0][0].toBool()); QCOMPARE(reload->requests, 0); QVERIFY(!QFile::exists(config));
        s = read(); put(config, "[Unrelated]\nChanged=true\n"); backend->writePreferences(s, p);
        QCOMPARE(finished.size(), 2); QVERIFY(!finished[1][0].toBool()); QCOMPARE(reload->requests, 0);
    }
    void invalidAndUnsupportedXmlNeverWrite_data() {
        QTest::addColumn<QByteArray>("data");
        QTest::newRow("broken") << QByteArray("<fontconfig>");
        QTest::newRow("unknown") << QByteArray("<fontconfig><match target=\"font\"><edit name=\"rgba\" mode=\"assign\"><const>unknown</const></edit></match></fontconfig>");
        QTest::newRow("complex unconditional") << QByteArray("<fontconfig><match target=\"font\"><edit name=\"rgba\" mode=\"append\"><const>rgb</const></edit></match></fontconfig>");
        QTest::newRow("entity") << QByteArray("<!DOCTYPE fontconfig [<!ENTITY x 'rgb'>]><fontconfig/>");
        QTest::newRow("hinting mismatch") << QByteArray("<fontconfig><match target=\"font\"><edit name=\"hinting\" mode=\"assign\"><bool>false</bool></edit></match></fontconfig>");
    }
    void invalidAndUnsupportedXmlNeverWrite() {
        QFETCH(QByteArray, data); auto initial = read(); put(xml, data);
        QSignalSpy reading(backend, &FontBackend::readFinished); backend->readCurrent(); QVERIFY(!reading[0][0].toBool()); QVERIFY(!reading[0][2].toString().isEmpty());
        auto p = initial.preferences; p.antialias = "true"; backend->writePreferences(initial, p);
        QCOMPARE(contents(xml), data); QVERIFY(!QFile::exists(config)); QCOMPARE(reload->requests, 0);
    }
    void patternTargetsAndDuplicateOverridesAreRefusedWithoutMutation_data() {
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<QString>("reason");
        const QList<QPair<QByteArray, QByteArray>> properties{{"antialias", "<bool>true</bool>"},
            {"hintstyle", "<const>hintslight</const>"}, {"rgba", "<const>rgb</const>"},
            {"hinting", "<bool>true</bool>"}};
        for (const auto &property : properties) {
            const QByteArray edit = "<edit name=\"" + property.first + "\" mode=\"assign\">" + property.second + "</edit>";
            QTest::newRow(("pattern-" + property.first).constData())
                << QByteArray("<fontconfig><match target=\"pattern\">") + edit + "</match></fontconfig>" << QStringLiteral("pattern-target");
            const QByteArray rule = "<match target=\"font\">" + edit + "</match>";
            QByteArray different = edit;
            different.replace(">true<", ">false<"); different.replace("hintslight", "hintfull"); different.replace(">rgb<", ">bgr<");
            const QByteArray lastRule = "<match target=\"font\">" + different + "</match>";
            // Intervening conditional rules must not make earlier assignments
            // disappear from the safety check or get silently reinterpreted.
            const QByteArray conditional = "<match target=\"font\"><test name=\"family\"><string>Special</string></test>" + edit + "</match>";
            QTest::newRow(("duplicate-" + property.first).constData())
                << QByteArray("<fontconfig>") + rule + conditional + lastRule + "</fontconfig>" << QStringLiteral("duplicate");
        }
    }
    void patternTargetsAndDuplicateOverridesAreRefusedWithoutMutation() {
        QFETCH(QByteArray, data); QFETCH(QString, reason);
        const auto initial = read(); put(xml, data);
        QSignalSpy reading(backend, &FontBackend::readFinished); backend->readCurrent();
        QVERIFY(!reading[0][0].toBool()); QVERIFY(reading[0][2].toString().contains(reason));
        auto requested = initial.preferences; requested.antialias = "false";
        QSignalSpy written(backend, &FontBackend::writeFinished); backend->writePreferences(initial, requested);
        QCOMPARE(written.size(), 1); QVERIFY(!written[0][0].toBool()); QVERIFY(written[0][1].toString().contains(reason));
        QCOMPARE(contents(xml), data); QVERIFY(!QFile::exists(config)); QCOMPARE(reload->requests, 0);
    }
    void unavailableAndFailedReloadReportedHonestly() {
        auto s = read(); auto p = s.preferences; p.antialias = "true";
        reload->available = false; backend->writePreferences(s, p); QVERIFY(!QFile::exists(xml));
        reload->available = true; reload->success = false;
        QSignalSpy finished(backend, &FontBackend::writeFinished); backend->writePreferences(s, p);
        QVERIFY(!finished[0][0].toBool()); QVERIFY(finished[0][1].toString().contains("Saved")); QCOMPARE(read().preferences, p);
    }
    void stalledReloadIsBounded() {
        auto s = read(); auto p = s.preferences; p.antialias = "false"; reload->complete = false;
        QSignalSpy finished(backend, &FontBackend::writeFinished); backend->writePreferences(s, p);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 4500); QVERIFY(!finished[0][0].toBool()); QVERIFY(finished[0][1].toString().contains("timed out"));
    }
    void atomicReplacementsRemainWatched() {
        auto s = read(); Q_UNUSED(s); QSignalSpy changed(backend, &FontBackend::externalChange);
        atomicPut(xml, "<fontconfig><!--first--></fontconfig>"); QTRY_VERIFY(!changed.isEmpty()); changed.clear();
        atomicPut(xml, "<fontconfig><!--second--></fontconfig>"); QTRY_VERIFY(!changed.isEmpty());
    }
private:
    FontState read() {
        QSignalSpy spy(backend, &FontBackend::readFinished); backend->readCurrent();
        if (spy.size() != 1 || !spy[0][0].toBool()) { QTest::qFail(qPrintable(spy.isEmpty() ? QStringLiteral("No read response") : spy[0][2].toString()), __FILE__, __LINE__); return {}; }
        return qvariant_cast<FontState>(spy[0][1]);
    }
    void put(const QString &path, const QByteArray &data) { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data), qint64(data.size())); }
    void atomicPut(const QString &path, const QByteArray &data) { QSaveFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data), qint64(data.size())); QVERIFY(f.commit()); }
    QByteArray contents(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
    QTemporaryDir temp;
    QString config, xml;
    FakeFontReload *reload = nullptr;
    KdeFontBackend *backend = nullptr;
};
QTEST_GUILESS_MAIN(FontBackendTest)
#include "font_backend_test.moc"
