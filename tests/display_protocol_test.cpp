#include <QtTest>
#include <limits>
#include "display_fixture.h"

class DisplayProtocolTest : public QObject {
    Q_OBJECT
private slots:
    void parsesKde66SchemaWithoutUuid()
    {
        const auto snapshot = displayFixture();
        QVERIFY(snapshot.isValid());
        QCOMPARE(snapshot.outputs.size(), 1);
        QCOMPARE(snapshot.outputs.first().name, QStringLiteral("eDP-1"));
        QCOMPARE(snapshot.outputs.first().modes.size(), 2);
        QString reason;
        QVERIFY(DisplayProtocol::canChange(snapshot, 1, "2", reason));
    }
    void strictJsonAndFixedArgvBoundary()
    {
        DisplaySnapshot snapshot; QString error;
        QVERIFY(!DisplayProtocol::parse("Output: 1 eDP-1", snapshot, error));
        QVERIFY(!DisplayProtocol::parse("{}", snapshot, error));
        QVERIFY(!DisplayProtocol::parse(QByteArray(1024 * 1024 + 1, ' '), snapshot, error));
        QVERIFY(DisplayProtocol::modeArgument(1, "2;reboot").isEmpty());
        QVERIFY(DisplayProtocol::modeArgument(1, "2.mode.3").isEmpty());
        QVERIFY(DisplayProtocol::modeArgument(-1, "2").isEmpty());
        QCOMPARE(DisplayProtocol::modeArgument(1, "2"), QStringLiteral("output.1.mode.2"));
        auto invalid = displayFixture().json;
        auto outputs = invalid.value("outputs").toArray();
        auto output = outputs[0].toObject(); output.remove("connected");
        outputs[0] = output; invalid.insert("outputs", outputs);
        QVERIFY(!DisplayProtocol::parse(QJsonDocument(invalid).toJson(), snapshot, error));
    }
    void modeReadbackRejectsUnrelatedChanges()
    {
        const auto prior = displayFixture();
        const auto preview = displayFixture("2");
        QVERIFY(!DisplayProtocol::same(prior, preview));
        QVERIFY(DisplayProtocol::matchesMode(prior, preview, 1, "2"));
        QVERIFY(!DisplayProtocol::matchesMode(prior, displayModified(preview, "scale", 2), 1, "2"));
        QVERIFY(!DisplayProtocol::matchesMode(prior, displayModified(preview, "name", "HDMI-A-1"), 1, "2"));
        QVERIFY(!DisplayProtocol::matchesMode(prior, displayModified(preview, "pos", QJsonObject{{"x", 100}, {"y", 0}}), 1, "2"));
    }
    void multipleOutputsShareOneCanonicalCandidate()
    {
        const auto baseline=displayMultiFixture(); auto draft=baseline;
        draft.output(1)->position=QPoint(-1280,100);
        draft.output(2)->position=QPoint(0,100);
        draft.output(1)->currentMode="2";
        draft.output(2)->scale=1.251;
        draft.output(2)->rotation=2;
        const auto target=DisplayProtocol::normalized(draft);QString error;
        QVERIFY2(DisplayProtocol::canChange(baseline,target,error),qPrintable(error));
        QCOMPARE(target.output(1)->position,QPoint(0,0));
        QCOMPARE(target.output(2)->position,QPoint(1280,0));
        QCOMPARE(target.output(2)->scale,1.25);
        const auto arguments=DisplayProtocol::changeArguments(baseline,target);
        QVERIFY(arguments.contains("output.eDP-1.mode.2"));
        QVERIFY(arguments.contains("output.HDMI-A-1.scale.1.25"));
        QVERIFY(arguments.contains("output.HDMI-A-1.rotation.left"));
        QVERIFY(arguments.contains("output.HDMI-A-1.position.1280,0"));
        QVERIFY(DisplayProtocol::matchesRequested(baseline,target,target));
        QVERIFY(!DisplayProtocol::matchesRequested(baseline,target,baseline));
    }
    void enablementAndNativeLimits()
    {
        const auto baseline=displayMultiFixture();auto target=baseline;QString error;
        target.output(1)->enabled=false;target=DisplayProtocol::normalized(target);
        QVERIFY(DisplayProtocol::canChange(baseline,target,error));
        QCOMPARE(target.output(2)->position,QPoint(0,0));
        QCOMPARE(target.output(2)->priority,1);
        QVERIFY(DisplayProtocol::changeArguments(baseline,target).contains("output.eDP-1.disable"));
        target.output(2)->enabled=false;target=DisplayProtocol::normalized(target);
        QVERIFY(!DisplayProtocol::canChange(baseline,target,error));
        QVERIFY(error.contains("At least one"));
        auto limited=baseline.json;auto screen=limited.value("screen").toObject();screen.insert("maxActiveOutputsCount",1);limited.insert("screen",screen);
        DisplaySnapshot one;QVERIFY(DisplayProtocol::parse(QJsonDocument(limited).toJson(),one,error));
        QVERIFY(!DisplayProtocol::canChange(one,baseline,error));
        auto disabled=baseline;disabled.output(2)->enabled=false;disabled=DisplayProtocol::normalized(disabled);
        QVERIFY(DisplayProtocol::canChange(disabled,baseline,error));
    }
    void partialOwnershipAndFreshColorRestoration()
    {
        const auto baseline=displayMultiFixture();auto target=baseline;
        target.output(1)->currentMode="2";target.output(2)->scale=1.5;target.output(2)->rotation=8;
        target=DisplayProtocol::normalized(target);
        auto partial=baseline;partial.output(1)->currentMode="2";partial=DisplayProtocol::normalized(partial);
        partial=displayModified(partial,"brightness",0.37);
        QVERIFY(DisplayProtocol::ownedIntermediate(baseline,target,partial));
        DisplaySnapshot restore;QString error;
        QVERIFY2(DisplayProtocol::rollbackRequest(baseline,target,partial,restore,error),qPrintable(error));
        QVERIFY(DisplayProtocol::same(restore,baseline));
        QCOMPARE(restore.json.value("outputs").toArray()[0].toObject().value("brightness").toDouble(),0.37);
        auto foreign=partial;foreign.output(2)->position=QPoint(2222,0);foreign=DisplayProtocol::normalized(foreign);
        QVERIFY(!DisplayProtocol::ownedIntermediate(baseline,target,foreign));
        QVERIFY(!DisplayProtocol::rollbackRequest(baseline,target,foreign,restore,error));
        QVERIFY(!DisplayProtocol::ownedIntermediate(baseline,target,displayModified(partial,"name","DP-8")));
    }
    void partialDisableKeepsNativeCompactedPriorities()
    {
        const auto baseline=displayMultiFixture();auto target=baseline;target.output(1)->enabled=false;
        target.output(2)->scale=1.5;target=DisplayProtocol::normalized(target);
        auto partial=baseline;partial.output(1)->enabled=false;partial=DisplayProtocol::normalized(partial);
        QVERIFY(DisplayProtocol::ownedIntermediate(baseline,target,partial));
        DisplaySnapshot restore;QString error;
        QVERIFY2(DisplayProtocol::rollbackRequest(baseline,target,partial,restore,error),qPrintable(error));
        QVERIFY(DisplayProtocol::same(restore,baseline));
    }
    void threeOutputPartialOriginShiftIsOwnedAsOneLayout()
    {
        const auto baseline=displayTripleFixture();auto target=baseline;
        target.output(1)->enabled=false;
        target.output(2)->position=QPoint(0,0);
        target.output(3)->position=QPoint(3000,0);
        target=DisplayProtocol::normalized(target);
        auto partial=baseline;partial.output(1)->enabled=false;
        partial=DisplayProtocol::normalized(partial);
        QCOMPARE(partial.output(2)->position,QPoint(0,0));
        QCOMPARE(partial.output(3)->position,QPoint(1920,0));
        QVERIFY(partial.output(3)->position!=baseline.output(3)->position);
        QVERIFY(partial.output(3)->position!=target.output(3)->position);
        QVERIFY(DisplayProtocol::ownedIntermediate(baseline,target,partial));
        DisplaySnapshot restore;QString error;
        QVERIFY2(DisplayProtocol::rollbackRequest(baseline,target,partial,restore,error),qPrintable(error));
        QVERIFY(DisplayProtocol::same(restore,baseline));
        for(const auto &foreignPosition:{QPoint(1937,0),QPoint(3001,0),QPoint(1920,17)}) {
            auto foreign=partial;foreign.output(3)->position=foreignPosition;
            foreign=DisplayProtocol::normalized(foreign);
            QVERIFY(!DisplayProtocol::ownedIntermediate(baseline,target,foreign));
            QVERIFY(!DisplayProtocol::rollbackRequest(baseline,target,foreign,restore,error));
        }
        // Disabled outputs are not translated by native normalization.
        auto movedDisabled=partial;movedDisabled.output(1)->position=QPoint(-1920,0);
        movedDisabled=DisplayProtocol::normalized(movedDisabled);
        QVERIFY(!DisplayProtocol::ownedIntermediate(baseline,target,movedDisabled));
        auto allOff=partial;for(auto &output:allOff.outputs)output.enabled=false;
        allOff=DisplayProtocol::normalized(allOff);
        QVERIFY(!DisplayProtocol::ownedIntermediate(baseline,target,allOff));
    }
    void nativePolicyAndFeatureGates()
    {
        auto baseline=displayMultiFixture();QString error;
        auto mirror=displayModified(baseline,"replicationSource",2);
        QVERIFY(!DisplayProtocol::canEdit(mirror,1,error));
        QVERIFY(!DisplayProtocol::canEdit(mirror,2,error));
        auto json=baseline.json;json.insert("features",255 & ~4);
        QVERIFY(DisplayProtocol::parse(QJsonDocument(json).toJson(),baseline,error));
        QVERIFY(!DisplayProtocol::canScale(baseline));
        auto target=baseline;target.output(2)->scale=2;target=DisplayProtocol::normalized(target);
        QVERIFY(!DisplayProtocol::canChange(baseline,target,error));
        json.insert("features",255 & ~64);QVERIFY(DisplayProtocol::parse(QJsonDocument(json).toJson(),baseline,error));
        QVERIFY(!DisplayProtocol::canPosition(baseline));QVERIFY(!DisplayProtocol::canRotate(baseline));
        auto malformed=displayModified(displayFixture(),"rotation",3);QVERIFY(!malformed.isValid());
        malformed=displayModified(displayFixture(),"scale",0);QVERIFY(!malformed.isValid());
        auto replaced=displayModified(displayFixture(),"modes",QJsonArray{});QVERIFY(!replaced.isValid());
    }
    void editedScaleQuantizesWithoutChangingSavedOtherOutput()
    {
        const auto baseline=displayModified(displayMultiFixture(),"scale",4.01);
        auto draft=baseline;draft.output(2)->scale=1.376;
        const auto target=DisplayProtocol::normalized(draft);QString error;
        QVERIFY(target.isValid());
        QCOMPARE(target.output(1)->scale,4.01);
        QCOMPARE(target.json.value("outputs").toArray()[0].toObject().value("scale").toDouble(),4.01);
        QCOMPARE(target.output(2)->scale,1.375);
        QVERIFY2(DisplayProtocol::canChange(baseline,target,error),qPrintable(error));
        const auto arguments=DisplayProtocol::changeArguments(baseline,target);
        QVERIFY(arguments.contains("output.HDMI-A-1.scale.1.375"));
        for(const auto &argument:arguments)QVERIFY(!argument.startsWith("output.eDP-1.scale."));
        const auto normalizedAgain=DisplayProtocol::normalized(target);
        QVERIFY(DisplayProtocol::same(target,normalizedAgain));
        auto foreign=displayModified(target,"scale",4.0100000001);
        QVERIFY(!DisplayProtocol::same(target,foreign));
        QVERIFY(!DisplayProtocol::ownedIntermediate(baseline,target,foreign));
        DisplaySnapshot restored;
        QVERIFY(DisplayProtocol::rollbackRequest(baseline,target,target,restored,error));
        QCOMPARE(restored.output(1)->scale,4.01);
        QVERIFY(DisplayProtocol::same(restored,baseline));
    }
    void nativeReadScaleBoundsPreserveOutOfEditorValues()
    {
        for(const double factor:{65.0,DisplayProtocol::MaximumReadScale}) {
            const auto baseline=displayModified(displayFixture(),"scale",factor);
            QVERIFY(baseline.isValid());
            QCOMPARE(DisplayProtocol::normalized(baseline).output(1)->scale,factor);
            auto draft=baseline;draft.output(1)->currentMode="2";
            const auto target=DisplayProtocol::normalized(draft);QString error;
            QVERIFY2(DisplayProtocol::canChange(baseline,target,error),qPrintable(error));
            QCOMPARE(DisplayProtocol::changeArguments(baseline,target),QStringList{"output.eDP-1.mode.2"});
        }
        QVERIFY(!displayModified(displayFixture(),"scale",DisplayProtocol::MaximumReadScale+1).isValid());
        QVERIFY(!displayModified(displayFixture(),"scale",DisplayProtocol::MinimumScale/2).isValid());
        const auto raw=displayModified(displayFixture(),"scale",1.2500000001);
        auto changed=raw;changed.output(1)->scale=1.25;changed=DisplayProtocol::normalized(changed);
        QString error;QVERIFY(!DisplayProtocol::same(raw,changed));
        QVERIFY(!DisplayProtocol::canChange(raw,changed,error));
        QVERIFY(error.contains("restored exactly"));
        auto ceiling=displayModified(displayFixture(),"scale",DisplayProtocol::MaximumReadScale);
        ceiling.output(1)->scale=1.5;ceiling=DisplayProtocol::normalized(ceiling);
        QVERIFY(!DisplayProtocol::canChange(displayModified(displayFixture(),"scale",DisplayProtocol::MaximumReadScale),ceiling,error));
    }
    void changedScaleMustFitExactNativeWriteAndRollbackBounds()
    {
        const auto baseline=displayFixture();QString error;
        for(const double factor:{DisplayProtocol::MinimumScale,DisplayProtocol::MaximumWrittenScale,1.375}) {
            auto target=baseline;target.output(1)->scale=factor;target=DisplayProtocol::normalized(target);
            QVERIFY(target.isValid());QCOMPARE(target.output(1)->scale,factor);
            QVERIFY2(DisplayProtocol::canChange(baseline,target,error),qPrintable(error));
            QVERIFY(!DisplayProtocol::changeArguments(baseline,target).isEmpty());
            DisplaySnapshot restore;QVERIFY(DisplayProtocol::rollbackRequest(baseline,target,target,restore,error));
            QVERIFY(DisplayProtocol::same(restore,baseline));
        }
        for(const double invalid:{DisplayProtocol::MinimumScale/4,DisplayProtocol::MaximumReadScale,
                                  std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
            auto target=baseline;target.output(1)->scale=invalid;
            QVERIFY(!DisplayProtocol::normalized(target).isValid());
        }
    }
    void fullPositionRangeNormalizesWithoutClampOrOverflow()
    {
        const auto baseline=displayMultiFixture();auto draft=baseline;
        draft.output(1)->position=QPoint(-DisplayProtocol::MaximumPosition,0);
        draft.output(2)->position=QPoint(0,0);
        const auto target=DisplayProtocol::normalized(draft);QString error;
        QVERIFY(target.isValid());
        QCOMPARE(target.output(1)->position,QPoint(0,0));
        QCOMPARE(target.output(2)->position,QPoint(DisplayProtocol::MaximumPosition,0));
        QVERIFY2(DisplayProtocol::canChange(baseline,target,error),qPrintable(error));
        QVERIFY(DisplayProtocol::changeArguments(baseline,target).contains("output.HDMI-A-1.position.1000000,0"));
        draft.output(2)->position=QPoint(1,0);QVERIFY(!DisplayProtocol::normalized(draft).isValid());
        draft=baseline;draft.output(2)->position=QPoint(std::numeric_limits<int>::max(),0);
        QVERIFY(!DisplayProtocol::normalized(draft).isValid());
        auto disabled=baseline;disabled.output(2)->enabled=false;disabled.output(2)->position=QPoint(-DisplayProtocol::MaximumPosition,-DisplayProtocol::MaximumPosition);
        disabled=DisplayProtocol::normalized(disabled);
        QVERIFY2(DisplayProtocol::canChange(baseline,disabled,error),qPrintable(error));
        QCOMPARE(disabled.output(2)->position,QPoint(-DisplayProtocol::MaximumPosition,-DisplayProtocol::MaximumPosition));
        auto limited=baseline.json;auto screen=limited.value("screen").toObject();screen.insert("maxSize",QJsonObject{{"width",64000},{"height",64000}});limited.insert("screen",screen);
        DisplaySnapshot bounded;QVERIFY(DisplayProtocol::parse(QJsonDocument(limited).toJson(),bounded,error));
        auto beyond=bounded;beyond.output(2)->position=QPoint(1000000,0);beyond=DisplayProtocol::normalized(beyond);
        QVERIFY(beyond.isValid());QVERIFY(!DisplayProtocol::canChange(bounded,beyond,error));QVERIFY(error.contains("size limit"));
    }
    void compositorIdentityIsPartOfBaseline()
    {
        auto baseline=displayFixture().json;
        baseline.insert("controlIdentity",QJsonObject{{"owner",":1.5"},{"kwinVersion","6.6.6"},{"libkscreenVersion","6.6.4"}});
        DisplaySnapshot prior;QString error;QVERIFY(DisplayProtocol::parse(QJsonDocument(baseline).toJson(),prior,error));
        auto replacement=baseline;auto identity=replacement.value("controlIdentity").toObject();identity.insert("owner",":1.6");replacement.insert("controlIdentity",identity);
        DisplaySnapshot actual;QVERIFY(DisplayProtocol::parse(QJsonDocument(replacement).toJson(),actual,error));
        QVERIFY(!DisplayProtocol::same(prior,actual));
        QVERIFY(!DisplayProtocol::ownedIntermediate(prior,prior,actual));
    }
    void automaticPreferredModeIsReadOnly()
    {
        const auto automatic = displayModified(displayFixture(), "followPreferredMode", true);
        QString error;
        QVERIFY(!DisplayProtocol::canChange(automatic, 1, "2", error));
        QVERIFY(error.contains("preferred mode"));
    }
    void unrelatedBrightnessDoesNotPreventRollback()
    {
        const auto prior = displayFixture();
        auto preview = displayModified(displayFixture("2"), "brightness", 0.5);
        QVERIFY(DisplayProtocol::matchesMode(prior, preview, 1, "2"));
    }
};
QTEST_GUILESS_MAIN(DisplayProtocolTest)
#include "display_protocol_test.moc"
