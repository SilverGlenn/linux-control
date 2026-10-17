#include "../src/data/ConfFile.h"
#include "../src/data/FirewallStatus.h"
#include "../src/data/UpdateCheckStatus.h"

#include <QtTest>
#include <QTemporaryDir>
#include <QFile>

static bool writeFixture(const QString &path, const QString &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    return file.write(contents.toUtf8()) == contents.toUtf8().size();
}

static QString readFixture(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

class StatusParsingTest : public QObject {
    Q_OBJECT

private slots:
    void readsQuotedAndUnquotedConfFields();
    void distinguishesMissingOrUnreadableConf();
    void parsesRecognizedUfwSettings();
    void parsesDisabledUfwSetting();
    void treatsMalformedAndMissingUfwSettingsAsUnknown();
    void countsConfiguredUfwRules();
    void classifiesRepositoryAndAurResults_data();
    void classifiesRepositoryAndAurResults();
};

void StatusParsingTest::readsQuotedAndUnquotedConfFields()
{
    const QString text = QStringLiteral(
        "# ENABLED=no\nENABLED=\"yes\"\nLOGLEVEL=high\nLOGLEVEL = low\n");
    const ConfField enabled = parseConfFieldText(text, QStringLiteral("ENABLED"));
    QVERIFY(enabled.available);
    QCOMPARE(enabled.value, QStringLiteral("yes"));

    const ConfField logLevel = parseConfFieldText(text, QStringLiteral("LOGLEVEL"));
    QVERIFY(logLevel.available);
    QCOMPARE(logLevel.value, QStringLiteral("high"));
    QVERIFY(!parseConfFieldText(QStringLiteral("LOGLEVEL = low\n"),
                                QStringLiteral("LOGLEVEL")).available);
}

void StatusParsingTest::distinguishesMissingOrUnreadableConf()
{
    QVERIFY(!parseConfFieldText(QStringLiteral("OTHER=value"),
                                QStringLiteral("ENABLED")).available);
    QVERIFY(!parseConfFieldText(QStringLiteral("ENABLED=yes"),
                                QStringLiteral("ENABLED"), false).available);
}

void StatusParsingTest::parsesRecognizedUfwSettings()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString ufwPath = dir.filePath(QStringLiteral("ufw.conf"));
    const QString defaultsPath = dir.filePath(QStringLiteral("default-ufw"));
    QVERIFY(writeFixture(ufwPath, QStringLiteral("ENABLED=yes\nLOGLEVEL=medium\n")));
    QVERIFY(writeFixture(defaultsPath, QStringLiteral(
        "DEFAULT_INPUT_POLICY=DROP\nDEFAULT_OUTPUT_POLICY=ACCEPT\n")));
    const UfwConfiguredState state = parseUfwConfiguredState(
        readFixture(ufwPath), true, readFixture(defaultsPath), true);

    QVERIFY(state.enabledKnown);
    QVERIFY(state.enabled);
    QVERIFY(state.inputPolicyKnown);
    QCOMPARE(state.inputPolicy, QStringLiteral("DROP"));
    QVERIFY(state.outputPolicyKnown);
    QCOMPARE(state.outputPolicy, QStringLiteral("ACCEPT"));
    QVERIFY(state.logLevelKnown);
    QCOMPARE(state.logLevel, QStringLiteral("medium"));
}

void StatusParsingTest::parsesDisabledUfwSetting()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString ufwPath = dir.filePath(QStringLiteral("ufw.conf"));
    QVERIFY(writeFixture(ufwPath, QStringLiteral("ENABLED=no\nLOGLEVEL=off\n")));
    const UfwConfiguredState state = parseUfwConfiguredState(
        readFixture(ufwPath), true, QString(), false);
    QVERIFY(state.enabledKnown);
    QVERIFY(!state.enabled);
    QVERIFY(state.logLevelKnown);
    QCOMPARE(state.logLevel, QStringLiteral("off"));
}

void StatusParsingTest::treatsMalformedAndMissingUfwSettingsAsUnknown()
{
    const UfwConfiguredState malformed = parseUfwConfiguredState(
        QStringLiteral("ENABLED=maybe\nLOGLEVEL=verbose\n"), true,
        QStringLiteral("DEFAULT_INPUT_POLICY=ALLOW\n"
                       "DEFAULT_OUTPUT_POLICY=\n"), true);
    QVERIFY(!malformed.enabledKnown);
    QVERIFY(!malformed.inputPolicyKnown);
    QVERIFY(!malformed.outputPolicyKnown);
    QVERIFY(!malformed.logLevelKnown);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString missingPath = dir.filePath(QStringLiteral("missing.conf"));
    QVERIFY(!readConfFieldStatus(missingPath, QStringLiteral("ENABLED")).available);
    const UfwConfiguredState missing = parseUfwConfiguredState(QString(), false,
                                                                 QString(), false);
    QVERIFY(!missing.enabledKnown);
    QVERIFY(!missing.inputPolicyKnown);
    QVERIFY(!missing.outputPolicyKnown);
    QVERIFY(!missing.logLevelKnown);
}

void StatusParsingTest::countsConfiguredUfwRules()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("user.rules"));
    QVERIFY(writeFixture(path, QStringLiteral(
        "### tuple ### allow tcp 22\n"
        "### tuple ### deny udp 53\n"
        "# comment\n")));
    const UfwRuleCount rules = parseUfwRuleCount(readFixture(path));
    QCOMPARE(rules.count, 2);

    QCOMPARE(parseUfwRuleCount(QString()).count, 0);
}

void StatusParsingTest::classifiesRepositoryAndAurResults_data()
{
    QTest::addColumn<bool>("repositorySucceeded");
    QTest::addColumn<int>("repositoryUpdates");
    QTest::addColumn<bool>("aurSucceeded");
    QTest::addColumn<int>("aurUpdates");
    QTest::addColumn<int>("expected");

    QTest::newRow("repository failure")
        << false << 0 << false << 0
        << static_cast<int>(PackageUpdateCheckState::RepositoryCheckFailed);
    QTest::newRow("repository updates, AUR unknown")
        << true << 3 << false << 0
        << static_cast<int>(PackageUpdateCheckState::UpdatesAvailableAurUnknown);
    QTest::newRow("no repository updates, AUR unknown")
        << true << 0 << false << 0
        << static_cast<int>(PackageUpdateCheckState::NoKnownUpdatesAurUnknown);
    QTest::newRow("both sources checked and empty")
        << true << 0 << true << 0
        << static_cast<int>(PackageUpdateCheckState::FullyUpToDate);
    QTest::newRow("AUR updates only")
        << true << 0 << true << 2
        << static_cast<int>(PackageUpdateCheckState::UpdatesAvailable);
}

void StatusParsingTest::classifiesRepositoryAndAurResults()
{
    QFETCH(bool, repositorySucceeded);
    QFETCH(int, repositoryUpdates);
    QFETCH(bool, aurSucceeded);
    QFETCH(int, aurUpdates);
    QFETCH(int, expected);
    QCOMPARE(static_cast<int>(classifyPackageUpdateCheck(
                 repositorySucceeded, repositoryUpdates,
                 aurSucceeded, aurUpdates)), expected);
}

QTEST_APPLESS_MAIN(StatusParsingTest)
#include "status_test.moc"
