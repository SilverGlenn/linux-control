#include <QtTest>
#include <KConfig>
#include <KConfigGroup>
#include <QFile>
#include <QTemporaryDir>
#include "settings/ShortcutScheme.h"

using namespace ShortcutScheme;
namespace {
ShortcutAction row(const QString &component,const QString &context,const QString &name,const QList<QKeySequence> &keys={})
{ShortcutAction a;a.id={component,context,name};a.current=keys;a.defaults={QKeySequence(QStringLiteral("Meta+F12"))};return a;}
ShortcutsSnapshot globals()
{
    ShortcutsSnapshot s;s.available=s.writable=true;s.owner=QStringLiteral(":1.313");s.interfaceSignature=QStringLiteral("private-global-owner-metadata");s.revision="private-revision";s.configRevision="private-config-baseline";s.layouts.owner=QStringLiteral(":1.919");
    s.actions={row(QStringLiteral("kwin"),QStringLiteral("default"),QStringLiteral("Window Close"),{QKeySequence(QStringLiteral("Alt+F4"))}),
        row(QStringLiteral("vendor.desktop"),QStringLiteral("default"),QStringLiteral("_launch"),{QKeySequence(QStringLiteral("Meta+J")),QKeySequence(QStringLiteral("Ctrl+K, Ctrl+J, Ctrl+M, Ctrl+N"))}),
        row(QStringLiteral("vendor.desktop"),QStringLiteral("editor"),QStringLiteral("_launch"),{QKeySequence(QStringLiteral("Meta+E"))}),
        row(QStringLiteral("vendor[tools]"),QStringLiteral("default"),QStringLiteral("Square [Bracket] = action"),{QKeySequence(QStringLiteral("Ctrl+,, Ctrl+K"))})};return s;
}
ShortcutsSnapshot standards()
{ShortcutsSnapshot s;s.available=s.writable=true;s.owner=QStringLiteral("native-standard-sdk-owner");s.actions={row(QStringLiteral("Edit"),QStringLiteral("default"),QStringLiteral("Copy"),{QKeySequence(QStringLiteral("Ctrl+C")),QKeySequence(QStringLiteral("Ctrl+Insert"))}),row(QStringLiteral("File"),QStringLiteral("default"),QStringLiteral("Open"),{QKeySequence(QStringLiteral("Ctrl+O"))})};return s;}
bool hasIssue(const QList<Issue> &issues,const QString &code)
{for(const auto &issue:issues)if(issue.code==code)return true;return false;}
QByteArray bytes(const QString &path)
{QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();}
}
class ShortcutSchemeTest final:public QObject {
    Q_OBJECT
private slots:
    void nativeKcmFixturePreservesBothScopesAndFullAlternatives(){
        QTemporaryDir dir;const auto path=dir.filePath(QStringLiteral("native.kksrc"));
        const QList<QKeySequence> alternatives{QKeySequence(QStringLiteral("Meta+J")),QKeySequence(QStringLiteral("Ctrl+K, Ctrl+J, Ctrl+M, Ctrl+N"))};
        {
            // Exact Plasma6.6.6 export APIs/group shapes, independently authored.
            KConfig config(path,KConfig::SimpleConfig);config.group("Settings").writeEntry("Name",QStringLiteral("Native fixture"));
            config.group("vendor.desktop").group("Global Shortcuts").writeEntry("_launch",QKeySequence::listToString(alternatives));
            config.group("StandardShortcuts").writeEntry("Copy",QKeySequence::listToString(standards().actions[0].current));
            config.group("Custom Commands").group("vendor.desktop").writeEntry("Exec",QStringLiteral("printf '%s' '$HOME'"));
            config.group("Custom Commands").group("vendor.desktop").writeEntry("Name",QStringLiteral("Literal command definition"));QVERIFY(config.sync());
        }
        const auto result=readFile(path);QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(result.document.name,QStringLiteral("Native fixture"));QCOMPARE(result.document.bindings.size(),2);QCOMPARE(result.document.commands.size(),1);QCOMPARE(result.document.commands[0].exec,QStringLiteral("printf '%s' '$HOME'"));
        auto plan=planImport(result.document,globals(),standards());QVERIFY(plan.valid);QCOMPARE(plan.globals.value(globals().actions[1].id),alternatives);QCOMPARE(plan.globals.value(globals().actions[1].id)[1].count(),4);QCOMPARE(plan.standards.value(standards().actions[0].id),standards().actions[0].current);QVERIFY(hasIssue(plan.issues,QStringLiteral("CommandDefinitionReview")));
    }
    void legacyBuiltinFixtureAllowsLocalizedNameAndNone(){
        const auto read=parse(QByteArray("[Settings]\nName=Windows Scheme (With Win Key)\nName[de]=Windows-Tasten\nName[zh_TW]=Windows\n\n[kwin][Global Shortcuts]\nWindow Close=Alt+F4\nSwitch to Desktop 2=none\n[StandardShortcuts]\nCopy=\n"));QVERIFY2(read.success,qPrintable(read.error));QCOMPARE(read.document.bindings.size(),3);
        for(const auto &binding:read.document.bindings)if(binding.id.action!=QLatin1String("Window Close"))QVERIFY(binding.keys.isEmpty());
        auto plan=planImport(read.document,globals(),standards());QVERIFY(plan.valid);QCOMPARE(plan.unresolved.size(),1);QCOMPARE(plan.unresolved[0].id.action,QStringLiteral("Switch to Desktop 2"));QVERIFY(hasIssue(plan.issues,QStringLiteral("UnknownAction")));
    }
    void allScopesAndNondefaultContextsRoundTripAtomically(){
        QTemporaryDir dir;const auto path=dir.filePath(QStringLiteral("all.kksrc"));const auto global=globals(),standard=standards();
        const auto exported=exportFile(path,QStringLiteral("Complete staged scheme"),global,standard,{},{{QStringLiteral("vendor.desktop"),QStringLiteral("Vendor command"),QStringLiteral("/usr/bin/printf 'literal % and $HOME'")}});QVERIFY2(exported.success,qPrintable(exported.error));QVERIFY(hasIssue(exported.issues,QStringLiteral("ContextExtension")));
        const auto serialized=bytes(path);QVERIFY(serialized.contains("[LinuxControl Scheme]"));QVERIFY(serialized.contains("ContextVersion=1"));QVERIFY(serialized.contains("[LinuxControl Context Shortcuts][vendor.desktop][editor]"));QVERIFY(serialized.contains("[StandardShortcuts]"));QVERIFY(!serialized.contains("private-global-owner-metadata"));QVERIFY(!serialized.contains(":1.313"));QVERIFY(!serialized.contains("private-revision"));QVERIFY(!serialized.contains("private-config-baseline"));QVERIFY(!serialized.contains(":1.919"));
        const auto read=readFile(path);QVERIFY2(read.success,qPrintable(read.error));QCOMPARE(read.document.bindings.size(),global.actions.size()+standard.actions.size());const auto plan=planImport(read.document,global,standard);QVERIFY2(plan.valid,qPrintable(plan.error));QVERIFY(plan.unresolved.isEmpty());QCOMPARE(plan.globals.size(),global.actions.size());QCOMPARE(plan.standards.size(),standard.actions.size());for(const auto &a:global.actions)QCOMPARE(plan.globals.value(a.id),a.current);for(const auto &a:standard.actions)QCOMPARE(plan.standards.value(a.id),a.current);
        // Native KCM-compatible default sections remain independently readable.
        KConfig native(path,KConfig::SimpleConfig);QCOMPARE(QKeySequence::listFromString(native.group("vendor.desktop").group("Global Shortcuts").readEntry("_launch")),global.actions[1].current);QCOMPARE(QKeySequence::listFromString(native.group("StandardShortcuts").readEntry("Copy")),standard.actions[0].current);
    }
    void selectedActionsComponentsAndScopesExportOnlyTheReviewSelection(){
        QTemporaryDir dir;const auto path=dir.filePath(QStringLiteral("selected.kksrc"));const auto global=globals(),standard=standards();Selection select;select.globalActions={global.actions[0].id};select.standardComponents={QStringLiteral("Edit")};
        auto result=exportFile(path,QStringLiteral("Selected"),global,standard,select,{{QStringLiteral("vendor.desktop"),QStringLiteral("Not selected"),QStringLiteral("echo retained only if selected")}});QVERIFY2(result.success,qPrintable(result.error));auto read=readFile(path);QCOMPARE(read.document.bindings.size(),2);QVERIFY(read.document.commands.isEmpty());
        select.globals=false;select.standardComponents.clear();select.standardActions={standard.actions[1].id};result=exportFile(path,QStringLiteral("Only Open"),global,standard,select);QVERIFY(result.success);read=readFile(path);QCOMPARE(read.document.bindings.size(),1);QCOMPARE(read.document.bindings[0].id.action,QStringLiteral("Open"));QVERIFY(read.document.bindings[0].scope==Scope::Standard);
    }
    void strictParsingRejectsMalformedDuplicateExpandedAndOverlongData_data(){
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("duplicate")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+A\na=Ctrl+B\n");
        QTest::newRow("duplicate group")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+A\n[kwin][Global Shortcuts]\nb=Ctrl+B\n");
        QTest::newRow("escaped duplicate")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+A\n\\x61=Ctrl+B\n");
        QTest::newRow("group alias duplicate")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+A\n[\\x6bwin][Global Shortcuts]\na=Ctrl+B\n");
        QTest::newRow("unknown key")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+NotARealKey\n");
        QTest::newRow("unknown modifier")<<QByteArray("[kwin][Global Shortcuts]\na=MadeUp+Ctrl+A\n");
        QTest::newRow("trailing comma separator")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+,,\n");
        QTest::newRow("trailing comma separator fourth chord")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+A, Ctrl+B, Ctrl+C, Ctrl+,,\n");
        QTest::newRow("five chords")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+A, Ctrl+B, Ctrl+C, Ctrl+D, Ctrl+E\n");
        QTest::newRow("blank alternative")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+A; ; Ctrl+B\n");
        QTest::newRow("duplicate alternative")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+A; Ctrl+A\n");
        QTest::newRow("expansion flag")<<QByteArray("[Custom Commands][test.desktop]\nExec[$e]=$HOME\nName=Test\n");
        QTest::newRow("immutable flag")<<QByteArray("[kwin][Global Shortcuts][$i]\na=Ctrl+A\n");
        QTest::newRow("deleted key")<<QByteArray("[kwin][Global Shortcuts]\na[$d]=Ctrl+A\n");
        QTest::newRow("localized assignment")<<QByteArray("[StandardShortcuts]\nCopy[de]=Ctrl+C\n");
        QTest::newRow("missing equals")<<QByteArray("[kwin][Global Shortcuts]\na\n");
        QTest::newRow("trailing header")<<QByteArray("[kwin][Global Shortcuts]garbage\na=Ctrl+A\n");
        QTest::newRow("unknown group")<<QByteArray("[Wrong]\na=Ctrl+A\n");
        QTest::newRow("unknown metadata")<<QByteArray("[Settings]\nOwner=:1.123\n[kwin][Global Shortcuts]\na=Ctrl+A\n");
        QTest::newRow("invalid escape")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+\\q\n");
        QTest::newRow("signed hex escape")<<QByteArray("[Custom Commands][test.desktop]\nName=Test\nExec=echo \\x+a\n");
        QTest::newRow("short hex escape")<<QByteArray("[kwin][Global Shortcuts]\na=Ctrl+\\x4\n");
        QTest::newRow("missing command name")<<QByteArray("[Custom Commands][test.desktop]\nExec=echo test\n");
        QTest::newRow("missing command exec")<<QByteArray("[Custom Commands][test.desktop]\nName=Test\n");
        QTest::newRow("unknown command field")<<QByteArray("[Custom Commands][test.desktop]\nName=Test\nExec=echo test\nType=Application\n");
        QTest::newRow("invalid command component")<<QByteArray("[Custom Commands][../test.desktop]\nName=Test\nExec=echo test\n");
        QTest::newRow("bad utf8 comment")<<QByteArray("#\xFF\n[kwin][Global Shortcuts]\na=Ctrl+A\n");
        QTest::newRow("empty")<<QByteArray();
        QTest::newRow("oversize")<<QByteArray(4*1024*1024+1,'#');
        QTest::newRow("oversize line")<<(QByteArray("[Settings]\nName=")+QByteArray(65537,'A'));
        QTest::newRow("oversize comment")<<(QByteArray("#")+QByteArray(65537,'A')+QByteArray("\n[kwin][Global Shortcuts]\na=Ctrl+A\n"));
        QByteArray many="[kwin][Global Shortcuts]\na=";for(int i=0;i<65;++i){if(i)many+="; ";many+="Ctrl+A";}QTest::newRow("too many alternatives")<<many;
    }
    void strictParsingRejectsMalformedDuplicateExpandedAndOverlongData(){QFETCH(QByteArray,input);const auto result=parse(input);QVERIFY(!result.success);QVERIFY(!result.error.isEmpty());QVERIFY(!result.issues.isEmpty());}
    void portablePunctuationModifierKeysAndLiteralDesktopEscapes(){
        const auto read=parse("[kwin][Global Shortcuts]\nComma=Ctrl+,\nPlus=Ctrl++\nSemicolon=Ctrl+;\nModifier=Meta\nCommaChord=Ctrl+,, Ctrl+K, Ctrl+,, Ctrl+,\n[Custom Commands][test.desktop]\nName=Literal escapes\nExec=printf '\\;\\,'\n");
        QVERIFY2(read.success,qPrintable(read.error));QCOMPARE(read.document.bindings.size(),5);QCOMPARE(read.document.commands[0].exec,QStringLiteral("printf '\\;\\,'"));
        for(const auto &binding:read.document.bindings)if(binding.id.action==QLatin1String("CommaChord")){QCOMPARE(binding.keys.size(),1);QCOMPARE(binding.keys[0].count(),4);QCOMPARE(binding.keys[0][2].key(),Qt::Key_Comma);}
    }
    void contextExtensionRequiresVersionAndNeverGuesses(){
        auto read=parse("[LinuxControl Context Shortcuts][vendor.desktop][other]\n_launch=Meta+J\n");QVERIFY(!read.success);QVERIFY(hasIssue(read.issues,QStringLiteral("Context")));
        read=parse("[LinuxControl Scheme]\nContextVersion=1\n[LinuxControl Context Shortcuts][vendor.desktop][other]\n_launch=Meta+J\n");QVERIFY(read.success);const auto plan=planImport(read.document,globals(),standards());QVERIFY(plan.valid);QCOMPARE(plan.unresolved.size(),1);QVERIFY(hasIssue(plan.issues,QStringLiteral("UnknownContext")));QVERIFY(plan.globals.isEmpty());
        read=parse("[LinuxControl Scheme]\nContextVersion=99\n[kwin][Global Shortcuts]\na=Meta+A\n");QVERIFY(!read.success);
        read=parse("[LinuxControl Scheme]\nContextVersion=1\n[LinuxControl Context Shortcuts][kwin][default]\na=Meta+A\n");QVERIFY(!read.success);
    }
    void customDefinitionsAndPendingBindingsAreNeverSilentlyDiscarded(){
        const auto read=parse("[Custom Commands][old.desktop]\nName=Imported command\nExec=printf '$HOME %s'\n[old.desktop][Global Shortcuts]\n_launch=Meta+J; Ctrl+K, Ctrl+J, Ctrl+M, Ctrl+N\n");QVERIFY2(read.success,qPrintable(read.error));const auto plan=planImport(read.document,globals(),standards());QVERIFY(plan.valid);QCOMPARE(plan.commands.size(),1);QCOMPARE(plan.unresolved.size(),1);QVERIFY(plan.globals.isEmpty());QVERIFY(hasIssue(plan.issues,QStringLiteral("PendingCommand")));QVERIFY(hasIssue(plan.issues,QStringLiteral("CommandDefinitionReview")));QCOMPARE(plan.commands[0].exec,QStringLiteral("printf '$HOME %s'"));QCOMPARE(plan.unresolved[0].keys[1].count(),4);
    }
    void explicitCustomCommandMigrationRemapsDefinitionsAndBindingsTogether(){
        const auto read=parse("[Custom Commands][old.desktop]\nName=Imported command\nExec=echo literal\n[old.desktop][Global Shortcuts]\n_launch=Meta+J\n");QVERIFY(read.success);const QMap<QString,QString> mapping{{QStringLiteral("old.desktop"),QStringLiteral("vendor.desktop")}};const auto plan=planImport(read.document,globals(),standards(),mapping);QVERIFY2(plan.valid,qPrintable(plan.error));QVERIFY(plan.unresolved.isEmpty());QCOMPARE(plan.commands[0].component,QStringLiteral("vendor.desktop"));QCOMPARE(plan.migrations,mapping);QCOMPARE(plan.globals.value(globals().actions[1].id),QList<QKeySequence>{QKeySequence(QStringLiteral("Meta+J"))});
        const auto invalid=planImport(read.document,globals(),standards(),{{QStringLiteral("not-defined.desktop"),QStringLiteral("vendor.desktop")}});QVERIFY(!invalid.valid);QVERIFY(hasIssue(invalid.issues,QStringLiteral("Migration")));
    }
    void ambiguousCommandMigrationAndSnapshotNamesFailVisibly(){
        auto read=parse("[Custom Commands][a.desktop]\nName=A\nExec=echo a\n[Custom Commands][b.desktop]\nName=B\nExec=echo b\n[a.desktop][Global Shortcuts]\n_launch=Meta+A\n[b.desktop][Global Shortcuts]\n_launch=Meta+B\n");QVERIFY(read.success);auto plan=planImport(read.document,globals(),standards(),{{QStringLiteral("a.desktop"),QStringLiteral("vendor.desktop")},{QStringLiteral("b.desktop"),QStringLiteral("vendor.desktop")}});QVERIFY(!plan.valid);QVERIFY(hasIssue(plan.issues,QStringLiteral("AmbiguousMigration")));
        auto standard=standards();standard.actions<<row(QStringLiteral("File"),QStringLiteral("default"),QStringLiteral("Copy"));read=parse("[StandardShortcuts]\nCopy=Ctrl+C\n");plan=planImport(read.document,globals(),standard);QVERIFY(!plan.valid);QVERIFY(hasIssue(plan.issues,QStringLiteral("AmbiguousSnapshot")));
        auto global=globals();global.actions<<global.actions[0];plan=planImport(read.document,global,standards());QVERIFY(!plan.valid);QVERIFY(hasIssue(plan.issues,QStringLiteral("AmbiguousSnapshot")));
    }
    void readonlyAndUnknownIdentitiesRemainReviewable(){
        auto global=globals();global.actions[0].available=false;const auto read=parse("[kwin][Global Shortcuts]\nWindow Close=Meta+Y\nUnknown=Meta+A\n[unknown][Global Shortcuts]\nOther=Meta+B\n[StandardShortcuts]\nNoSuchStandardAction=Ctrl+Q\n");QVERIFY(read.success);const auto plan=planImport(read.document,global,standards());QVERIFY(plan.valid);QCOMPARE(plan.unresolved.size(),4);QVERIFY(plan.globals.isEmpty());QVERIFY(plan.standards.isEmpty());QVERIFY(hasIssue(plan.issues,QStringLiteral("ReadOnly")));QVERIFY(hasIssue(plan.issues,QStringLiteral("UnknownComponent")));QVERIFY(hasIssue(plan.issues,QStringLiteral("UnknownAction")));
    }
    void matchingImmutableAssignmentsAndUnavailableScopesAreReported(){
        auto global=globals(),standard=standards();global.actions[0].available=false;standard.writable=false;
        const auto read=parse("[kwin][Global Shortcuts]\nWindow Close=Alt+F4\n[StandardShortcuts]\nCopy=Ctrl+C; Ctrl+Insert\n");QVERIFY(read.success);
        const auto plan=planImport(read.document,global,standard);QVERIFY(plan.valid);QCOMPARE(plan.unresolved.size(),2);QVERIFY(plan.globals.isEmpty());QVERIFY(plan.standards.isEmpty());
        QCOMPARE(plan.issues.size(),2);for(const auto &issue:plan.issues)QCOMPARE(issue.code,QStringLiteral("ReadOnly"));
    }
    void unresolvedEntriesAndAlternativesAreNotTruncated(){
        QByteArray input="[missing][Global Shortcuts]\n";
        constexpr int count=250;for(int i=0;i<count;++i)input+="Action"+QByteArray::number(i)+"=Meta+J; Ctrl+K, Ctrl+J, Ctrl+M, Ctrl+N\n";
        const auto read=parse(input);QVERIFY2(read.success,qPrintable(read.error));QCOMPARE(read.document.bindings.size(),count);
        const auto plan=planImport(read.document,globals(),standards());QVERIFY(plan.valid);QCOMPARE(plan.unresolved.size(),count);QCOMPARE(plan.issues.size(),count);
        for(const auto &binding:plan.unresolved){QCOMPARE(binding.keys.size(),2);QCOMPARE(binding.keys[1].count(),4);}
    }
    void invalidProgrammaticDocumentsAndMigrationCollisionsFail(){
        Document document;document.bindings.append({static_cast<Scope>(99),{QStringLiteral("kwin"),QStringLiteral("default"),QStringLiteral("Window Close")},{QKeySequence(QStringLiteral("Alt+F4"))}});
        auto plan=planImport(document,globals(),standards());QVERIFY(!plan.valid);QVERIFY(hasIssue(plan.issues,QStringLiteral("Scope")));
        const auto read=parse("[Custom Commands][old.desktop]\nName=Old\nExec=echo old\n[old.desktop][Global Shortcuts]\n_launch=Meta+J\n[vendor.desktop][Global Shortcuts]\n_launch=Meta+K\n");QVERIFY(read.success);
        plan=planImport(read.document,globals(),standards(),{{QStringLiteral("old.desktop"),QStringLiteral("vendor.desktop")}});QVERIFY(!plan.valid);QVERIFY(hasIssue(plan.issues,QStringLiteral("AmbiguousMigration")));
    }
    void invalidExportsPreserveExistingFileAndRejectNativePreferenceTargets(){
        QTemporaryDir dir;const auto path=dir.filePath(QStringLiteral("keep.kksrc"));{QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("preserve exact original bytes");}
        auto global=globals();global.actions[0].current={QKeySequence(int(Qt::Key_unknown))};const auto result=exportFile(path,QStringLiteral("Invalid"),global,standards());QVERIFY(!result.success);QCOMPARE(bytes(path),QByteArray("preserve exact original bytes"));
        const auto native=exportFile(dir.filePath(QStringLiteral("kglobalshortcutsrc")),QStringLiteral("Wrong destination"),globals(),standards());QVERIFY(!native.success);QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("kglobalshortcutsrc"))));
        const auto link=dir.filePath(QStringLiteral("linked.kksrc"));QVERIFY(QFile::link(path,link));QVERIFY(!exportFile(link,QStringLiteral("Linked"),globals(),standards()).success);QCOMPARE(bytes(path),QByteArray("preserve exact original bytes"));
        Selection selection;selection.globalActions={{QStringLiteral("missing"),QStringLiteral("default"),QStringLiteral("missing")}};selection.standards=false;QVERIFY(!exportFile(path,QStringLiteral("Stale selection"),globals(),standards(),selection).success);QCOMPARE(bytes(path),QByteArray("preserve exact original bytes"));
    }
    void builtinDiscoveryUsesNativeKcmkeysRootsAndReportsInvalidFiles(){
        QTemporaryDir dir;QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("kcmkeys"))));
        {QFile file(dir.filePath(QStringLiteral("kcmkeys/native.kksrc")));QVERIFY(file.open(QIODevice::WriteOnly));file.write("[Settings]\nName=Native root scheme\n[kwin][Global Shortcuts]\nWindow Close=Alt+F4\n");}
        {QFile file(dir.filePath(QStringLiteral("kcmkeys/invalid.kksrc")));QVERIFY(file.open(QIODevice::WriteOnly));file.write("broken");}
        {QFile file(dir.filePath(QStringLiteral("ignored.kksrc")));QVERIFY(file.open(QIODevice::WriteOnly));file.write("outside native subdir");}
        const auto schemes=builtInSchemes({dir.path()});QCOMPARE(schemes.size(),2);bool named=false,invalid=false;for(const auto &scheme:schemes){if(scheme.name==QLatin1String("Native root scheme")){named=true;QVERIFY(scheme.issues.isEmpty());}if(scheme.path.endsWith(QLatin1String("invalid.kksrc"))){invalid=true;QVERIFY(!scheme.issues.isEmpty());}}QVERIFY(named);QVERIFY(invalid);
    }
};
QTEST_MAIN(ShortcutSchemeTest)
#include "shortcut_scheme_test.moc"
