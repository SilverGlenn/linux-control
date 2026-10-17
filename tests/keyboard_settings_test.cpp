#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <limits>
#include "settings/KeyboardSettings.h"

namespace {
KeyboardSnapshot fixture()
{
    KeyboardSnapshot state;
    state.owner=QStringLiteral(":1.42");state.version=QStringLiteral("6.6.6");
    state.available=state.writable=state.reloadAvailable=true;
    state.values=KeyboardTypes::defaults();state.values["VendorFutureKey"]=QStringLiteral("preserve");
    state.registry.valid=true;state.registry.revision=QByteArrayLiteral("registry-v1");state.revision=QByteArrayLiteral("saved-v1");
    state.registry.models={{QStringLiteral("pc104"),QStringLiteral("Generic 104")},{QStringLiteral("pc105"),QStringLiteral("Generic 105")}};
    state.registry.layouts={{QStringLiteral("us"),QStringLiteral("English US"),{{QStringLiteral("dvorak"),QStringLiteral("Dvorak")}}}};
    state.registry.optionGroups={
        {QStringLiteral("caps"),QStringLiteral("Caps Lock"),false,{{QStringLiteral("caps:escape"),QStringLiteral("Escape")},{QStringLiteral("caps:ctrl"),QStringLiteral("Control")},{QStringLiteral("caps:none"),QStringLiteral("Disabled")}}},
        {QStringLiteral("compose"),QStringLiteral("Compose"),true,{{QStringLiteral("compose:ralt"),QStringLiteral("Right Alt")},{QStringLiteral("compose:lwin"),QStringLiteral("Left Meta")}}}};
    state.runtimeStatus=QStringLiteral("Saved and read back; runtime adoption not exposed.");return state;
}
void customLayouts(KeyboardSnapshot &state)
{
    state.values["Use"]=true;state.values["LayoutList"]=QStringList{QStringLiteral("us")};
    state.values["VariantList"]=QStringList{QStringLiteral("dvorak")};state.values["DisplayNames"]=QStringList{QStringLiteral("DV")};
}
const QByteArray registryXml=QByteArrayLiteral("<xkbConfigRegistry><modelList><model><configItem><name>pc104</name><description>Generic 104</description></configItem></model></modelList><layoutList><layout><configItem><name>us</name><description>English US</description></configItem><variantList><variant><configItem><name>dvorak</name><description>Dvorak</description></configItem></variant></variantList></layout></layoutList><optionList><group allowMultipleSelection=\"false\"><configItem><name>caps</name><description>Caps</description></configItem><option><configItem><name>caps:escape</name><description>Escape</description></configItem></option></group></optionList></xkbConfigRegistry>");
}
class FakeKeyboardBackend final:public KeyboardBackend {
public:
    KeyboardSnapshot current=fixture(),baseline;
    QVariantMap requested;
    int reads=0,writes=0;
    bool autoRead=true,autoWrite=true;
    quint64 readToken=0,writeToken=0;
    void readCurrent(quint64 token)override {
        ++reads;readToken=token;if(autoRead)emit readFinished(token,true,current,QString());
    }
    void writePreferences(quint64 token,const KeyboardSnapshot &expected,const QVariantMap &values)override {
        ++writes;writeToken=token;baseline=expected;requested=values;
        if(autoWrite){current.values=values;current.revision=QByteArrayLiteral("saved-v2");emit writeFinished(token,KeyboardOutcome::Success,current,QString());}
    }
    void reportRead(quint64 token,bool success=true,const QString &error={}){emit readFinished(token,success,current,error);}
    void reportWrite(KeyboardOutcome outcome,const KeyboardSnapshot &report,const QString &error={}){emit writeFinished(writeToken,outcome,report,error);}
    void changed(){emit externalChange();}
};
class KeyboardSettingsTest final:public QObject {
    Q_OBJECT
private slots:
    void registryRejectsTrailingXmlAndInvalidIds() {
        QTemporaryDir dir;const auto path=dir.filePath(QStringLiteral("evdev.xml"));
        auto save=[&](const QByteArray &bytes){QFile file(path);if(!file.open(QIODevice::WriteOnly))return false;return file.write(bytes)==bytes.size();};
        QVERIFY(save(registryXml));auto registry=KeyboardTypes::readRegistry(path);QVERIFY2(registry.valid,qPrintable(registry.error));
        QCOMPARE(registry.models.size(),1);QVERIFY(registry.layout("us"));QCOMPARE(registry.layout("us")->variants.first().id,QStringLiteral("dvorak"));
        const auto revision=registry.revision;
        QVERIFY(save(registryXml+"<extra/>"));registry=KeyboardTypes::readRegistry(path);QVERIFY(!registry.valid);QVERIFY(!registry.error.isEmpty());
        QVERIFY(save(registryXml+"trailing text"));QVERIFY(!KeyboardTypes::readRegistry(path).valid);
        auto invalid=registryXml;invalid.replace("<name>pc104</name>","<name>../pc104</name>");QVERIFY(save(invalid));QVERIFY(!KeyboardTypes::readRegistry(path).valid);
        QVERIFY(save(registryXml+"\n"));registry=KeyboardTypes::readRegistry(path);QVERIFY(registry.valid);QVERIFY(revision!=registry.revision);
    }
    void registryMergesExoticVariantsAndOptions() {
        QTemporaryDir dir;const auto primary=dir.filePath("evdev.xml"),extras=dir.filePath("evdev.extras.xml");
        {QFile file(primary);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(registryXml),qint64(registryXml.size()));}
        const auto extra=QByteArrayLiteral("<xkbConfigRegistry><layoutList><layout><configItem><name>us</name></configItem><variantList><variant><configItem><name>workman</name><description>Workman</description></configItem></variant></variantList></layout></layoutList><optionList><group allowMultipleSelection=\"false\"><configItem><name>caps</name></configItem><option><configItem><name>caps:ctrl</name><description>Control</description></configItem></option></group></optionList></xkbConfigRegistry>");
        {QFile file(extras);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(extra),qint64(extra.size()));}
        const auto registry=KeyboardTypes::readRegistry(primary,extras);QVERIFY2(registry.valid,qPrintable(registry.error));QCOMPARE(registry.layout("us")->variants.size(),2);QCOMPARE(registry.optionGroups.first().options.size(),2);
    }
    void strictTypesRangesAndUnknownKeys() {
        const auto state=fixture();QString error;
        const QList<QPair<QString,QVariant>> invalid={
            {"RepeatDelay",QStringLiteral("600")},{"RepeatRate",25},{"NumLock",QStringLiteral("2")},
            {"Use",QStringLiteral("false")},{"ResetOldOptions",0},{"kbdLayoutChangedEnabled",QStringLiteral("true")},
            {"Options",QVariantList{}},{"LayoutList",QStringLiteral("")},{"VariantList",QStringLiteral("")},{"DisplayNames",QStringLiteral("")},
            {"Model",104},{"KeyRepeat",true},{"SwitchMode",0},{"LayoutLoopCount",-1.0},
            {"RepeatDelay",99},{"RepeatDelay",5001},{"RepeatRate",0.1},{"RepeatRate",201.0},{"RepeatRate",std::numeric_limits<double>::infinity()},
            {"NumLock",3},{"KeyRepeat",QStringLiteral("none")},{"SwitchMode",QStringLiteral("Application")},{"Model",QStringLiteral("invented")}};
        for(const auto &entry:invalid){auto values=state.values;values[entry.first]=entry.second;QVERIFY2(!KeyboardTypes::validate(state,values,error),qPrintable(entry.first));}
        auto values=state.values;values["NewUnknown"]=true;QVERIFY(!KeyboardTypes::validate(state,values,error));
        values=state.values;values.remove("VendorFutureKey");QVERIFY(!KeyboardTypes::validate(state,values,error));
        values=state.values;values["VendorFutureKey"]=QStringLiteral("changed");QVERIFY(!KeyboardTypes::validate(state,values,error));
        values=state.values;values["RepeatDelay"]=100;values["RepeatRate"]=0.2;QVERIFY(KeyboardTypes::validate(state,values,error));
        values["RepeatDelay"]=5000;values["RepeatRate"]=200.0;QVERIFY(KeyboardTypes::validate(state,values,error));
    }
    void canonicalListsPreserveUnknownSavedPreferences() {
        auto state=fixture();QString error;auto values=state.values;
        values["VariantList"]=QStringList{QString()};values["DisplayNames"]=QStringList{QString(),QString()};
        QVERIFY(KeyboardTypes::sameValues(state.values,values));QVERIFY(KeyboardTypes::changedKeys(state.values,values).isEmpty());QVERIFY(KeyboardTypes::validate(state,values,error));
        state.values["Model"]=QStringLiteral("vendor-model");state.values["KeyRepeat"]=QStringLiteral("vendor-repeat");state.values["Options"]=QStringList{QStringLiteral("vendor:future")};
        values=state.values;values["RepeatDelay"]=850;QVERIFY2(KeyboardTypes::validate(state,values,error),qPrintable(error));
        QCOMPARE(values.value("Options").toStringList(),QStringList{QStringLiteral("vendor:future")});
        values["Options"]=QStringList{QStringLiteral("vendor:future"),QStringLiteral("compose:ralt")};QVERIFY(KeyboardTypes::validate(state,values,error));
        values["Options"]=QStringList{QStringLiteral("vendor:new")};QVERIFY(!KeyboardTypes::validate(state,values,error));
    }
    void exclusiveOptionGroupsValidateActualSelections() {
        auto state=fixture();QString error;auto values=state.values;
        values["Options"]=QStringList{QStringLiteral("caps:escape"),QStringLiteral("caps:ctrl")};QVERIFY(!KeyboardTypes::validate(state,values,error));
        values["Options"]=QStringList{QStringLiteral("compose:ralt"),QStringLiteral("compose:lwin")};QVERIFY(KeyboardTypes::validate(state,values,error));
        state.values["Options"]=QStringList{QStringLiteral("caps:escape"),QStringLiteral("caps:ctrl")};
        values=state.values;values["Options"]=QStringList{QStringLiteral("caps:escape"),QStringLiteral("caps:ctrl"),QStringLiteral("compose:ralt")};QVERIFY(KeyboardTypes::validate(state,values,error));
        values["Options"]=QStringList{QStringLiteral("caps:escape"),QStringLiteral("caps:none")};QVERIFY(!KeyboardTypes::validate(state,values,error));
        values["Options"]=QStringList{QStringLiteral("caps:none")};QVERIFY(KeyboardTypes::validate(state,values,error));
        values["Options"]=QStringList{QStringLiteral("caps:none"),QStringLiteral("caps:none")};QVERIFY(!KeyboardTypes::validate(state,values,error));
    }
    void disablingCustomLayoutsRequiresClearingNativeLists() {
        auto state=fixture();customLayouts(state);QString error;auto values=state.values;values["Use"]=false;
        QVERIFY(!KeyboardTypes::validate(state,values,error));
        values["LayoutList"]=QStringList{};values["VariantList"]=QStringList{};values["DisplayNames"]=QStringList{};QVERIFY(KeyboardTypes::validate(state,values,error));
        auto *backend=new FakeKeyboardBackend;backend->current=state;KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());
        session.selectValue("Use",false);QVERIFY(session.isDirty());QVERIFY(!session.staged().values.value("Use").toBool());
        for(const auto *key:{"LayoutList","VariantList","DisplayNames"})QVERIFY(session.staged().values.value(QLatin1String(key)).toStringList().isEmpty());
        QCOMPARE(backend->writes,0);
        session.revert();session.selectValues({{"LayoutList",QStringList{}}});QVERIFY(!session.staged().values.value("Use").toBool());
    }
    void defaultsRespectImmutableCoupledFields() {
        auto *backend=new FakeKeyboardBackend;customLayouts(backend->current);backend->current.values["RepeatDelay"]=850;backend->current.values["Model"]=QStringLiteral("pc105");
        backend->current.immutableKeys={QStringLiteral("LayoutList"),QStringLiteral("Model")};
        KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());session.restoreDefaults();
        QCOMPARE(session.staged().values.value("RepeatDelay").toInt(),600);QCOMPARE(session.staged().values.value("Model").toString(),QStringLiteral("pc105"));
        QVERIFY(session.staged().values.value("Use").toBool());QCOMPARE(session.staged().values.value("LayoutList"),backend->current.values.value("LayoutList"));
        QCOMPARE(session.staged().values.value("VariantList"),backend->current.values.value("VariantList"));QCOMPARE(session.staged().values.value("DisplayNames"),backend->current.values.value("DisplayNames"));
        QCOMPARE(session.staged().values.value("VendorFutureKey").toString(),QStringLiteral("preserve"));QVERIFY(session.isDirty());
        session.selectValue("Use",false);QVERIFY(session.staged().values.value("Use").toBool());QVERIFY(!session.errorText().isEmpty());
    }
    void successfulApplyUsesReturnedReadbackAndPersistsUnknown() {
        auto *backend=new FakeKeyboardBackend;KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());
        session.selectValue("RepeatDelay",850);QVERIFY(session.isDirty());QCOMPARE(backend->writes,0);
        QSignalSpy done(&session,&SettingsSession::applyFinished);session.apply();QCOMPARE(backend->writes,1);QCOMPARE(done.size(),1);QVERIFY(done.first().first().toBool());
        QCOMPARE(backend->baseline.values.value("RepeatDelay").toInt(),600);QCOMPARE(backend->requested.value("VendorFutureKey").toString(),QStringLiteral("preserve"));
        QCOMPARE(session.applied().values.value("RepeatDelay").toInt(),850);QVERIFY(!session.isDirty());QVERIFY(!session.hasConflict());QVERIFY(session.resultText().contains("saved and read back"));
    }
    void dirtyRefreshConflictsAndRevertReconciles() {
        auto *backend=new FakeKeyboardBackend;KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());session.selectValue("RepeatDelay",850);
        backend->current.values["RepeatRate"]=30.0;backend->current.revision="external";backend->changed();
        QVERIFY(session.hasConflict());QCOMPARE(session.staged().values.value("RepeatDelay").toInt(),850);QCOMPARE(session.applied().values.value("RepeatRate").toDouble(),30.0);
        session.apply();QCOMPARE(backend->writes,0);session.revert();QVERIFY(!session.hasConflict());QVERIFY(!session.isDirty());QCOMPARE(session.staged().values.value("RepeatRate").toDouble(),30.0);
    }
    void dirtyRefreshDetectsPolicyAndRegistryEvenWithoutValueChange() {
        auto *backend=new FakeKeyboardBackend;KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());session.selectValue("RepeatDelay",850);
        backend->current.immutableKeys.insert("RepeatDelay");backend->changed();QVERIFY(session.hasConflict());session.revert();QVERIFY(!session.hasConflict());
        backend->current.immutableKeys.clear();backend->changed();session.selectValue("RepeatDelay",900);backend->current.registry.revision="registry-v2";backend->changed();QVERIFY(session.hasConflict());
    }
    void knownPartialRequiresRevertWithoutOwnerRestart() {
        auto *backend=new FakeKeyboardBackend;backend->autoWrite=false;KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());
        session.selectValue("RepeatDelay",850);session.selectValue("RepeatRate",31.5);session.apply();backend->current.values["RepeatDelay"]=850;
        backend->reportWrite(KeyboardOutcome::Partial,backend->current,QStringLiteral("Second file changed."));
        QVERIFY(session.hasConflict());QVERIFY(session.isDirty());QVERIFY(!session.canApply());QCOMPARE(session.staged().values.value("RepeatRate").toDouble(),31.5);
        session.revert();QVERIFY(session.canApply());QVERIFY(!session.hasConflict());QCOMPARE(session.staged().values.value("RepeatDelay").toInt(),850);
    }
    void uncertainSameOwnerLockAndReplacementUnlock() {
        auto *backend=new FakeKeyboardBackend;backend->autoWrite=false;KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());session.selectValue("RepeatDelay",850);session.apply();
        backend->reportWrite(KeyboardOutcome::Uncertain,backend->current);QVERIFY(session.hasConflict());QVERIFY(!session.canApply());session.revert();QVERIFY(!session.canApply());
        const auto writes=backend->writes;session.selectValue("RepeatDelay",900);session.apply();QCOMPARE(backend->writes,writes);
        backend->current.owner=":1.99";backend->changed();QVERIFY(session.canApply());session.selectValue("RepeatDelay",900);QVERIFY(session.isDirty());
    }
    void wrongSuccessCannotDiscardDraft() {
        auto *backend=new FakeKeyboardBackend;backend->autoWrite=false;KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());session.selectValue("RepeatDelay",850);session.apply();
        QSignalSpy done(&session,&SettingsSession::applyFinished);backend->reportWrite(KeyboardOutcome::Success,backend->current);
        QCOMPARE(done.size(),1);QVERIFY(!done.first().first().toBool());QVERIFY(session.isDirty());QVERIFY(session.hasConflict());QVERIFY(!session.canApply());QVERIFY(session.resultText().isEmpty());
        session.revert();QVERIFY(!session.canApply());backend->current.owner=":1.200";backend->changed();QVERIFY(session.canApply());
    }
    void wrongSuccessVersionRejectedWithMatchingValues() {
        auto *backend=new FakeKeyboardBackend;backend->autoWrite=false;KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());session.selectValue("RepeatDelay",850);session.apply();
        auto report=backend->current;report.values=backend->requested;report.version=QStringLiteral("6.7.0");backend->reportWrite(KeyboardOutcome::Success,report);
        QVERIFY(session.hasConflict());QVERIFY(!session.canApply());QVERIFY(session.resultText().isEmpty());
    }
    void staleTokensAndRefreshDuringApplyAreBounded() {
        auto *backend=new FakeKeyboardBackend;KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());
        backend->autoRead=false;session.refresh();QVERIFY(session.isBusy());const auto token=backend->readToken;
        backend->reportRead(token-1);QVERIFY(session.isBusy());backend->reportRead(token);QVERIFY(!session.isBusy());
        backend->autoWrite=false;session.selectValue("RepeatDelay",850);session.apply();QVERIFY(session.isBusy());
        emit backend->writeFinished(backend->writeToken-1,KeyboardOutcome::Success,backend->current,QString());QVERIFY(session.isBusy());
        session.revert();session.selectValue("RepeatDelay",900);QCOMPARE(session.staged().values.value("RepeatDelay").toInt(),850);
        const int reads=backend->reads;backend->changed();backend->changed();backend->current.values=backend->requested;backend->reportWrite(KeyboardOutcome::Success,backend->current);
        QTRY_COMPARE(backend->reads,reads+1);backend->reportRead(backend->readToken);QVERIFY(!session.isBusy());QVERIFY(!session.isDirty());
    }
    void failedReadRetainsDraftAndBlocksApply() {
        auto *backend=new FakeKeyboardBackend;KeyboardSettings session(backend);QTRY_VERIFY(session.isReady());session.selectValue("RepeatDelay",850);
        backend->autoRead=false;session.refresh();backend->reportRead(backend->readToken,false,QStringLiteral("Disconnected"));
        QVERIFY(!session.isReady());QVERIFY(session.hasConflict());QVERIFY(!session.canApply());QCOMPARE(session.staged().values.value("RepeatDelay").toInt(),850);QCOMPARE(session.errorText(),QStringLiteral("Disconnected"));
    }
};
QTEST_GUILESS_MAIN(KeyboardSettingsTest)
#include "keyboard_settings_test.moc"
