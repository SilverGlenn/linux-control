#include <QtTest>
#include <QSignalSpy>
#include "settings/ThemeComponentSettings.h"

class FakeThemeComponentBackend : public ThemeComponentBackend {
public:
    int reads = 0;
    int writes = 0;
    QStringList expected;
    QStringList requested;
    void readCurrent() override { ++reads; }
    void writeComponents(const QStringList &old, const QStringList &next) override
    { ++writes; expected = old; requested = next; }
    static ThemeComponentState state(const QStringList &values = {"breeze", "default", "Breeze"})
    {
        ThemeComponentState result;
        const QStringList next{"Windows 7 Aero", "aero-drop", "kvantum"};
        for (int i = 0; i < themeComponentCount; ++i) {
            result.fields[i].value = values[i];
            result.fields[i].writable = result.fields[i].reloadAvailable = true;
            result.fields[i].choices = {{QStringList{"breeze", "default", "Breeze"}[i], "Original"}, {next[i], "Alternative"}};
        }
        return result;
    }
    void report(const ThemeComponentState &value = FakeThemeComponentBackend::state()) { emit readFinished(true, value, {}); }
    void failRead() { emit readFinished(false, {}, "Read unavailable"); }
    void written(bool success = true) { emit writeFinished(success, success ? QString() : QStringLiteral("Reload failed after saving")); }
    void changed() { emit externalChange(); }
};

class ThemeComponentSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        backend = new FakeThemeComponentBackend;
        session = new ThemeComponentSettings(backend);
        QTRY_COMPARE(backend->reads, 1);
        backend->report();
    }
    void cleanup() { delete session; }
    void approvedSelectionIsStagedWithoutWriting()
    {
        session->selectComponent(ThemeComponent::Icons, "Windows 7 Aero");
        QCOMPARE(session->stagedValue(ThemeComponent::Icons), "Windows 7 Aero");
        QCOMPARE(session->stagedValue(ThemeComponent::Cursors), "default");
        QCOMPARE(session->stagedValue(ThemeComponent::ApplicationStyle), "Breeze");
        QVERIFY(session->isDirty());
        QCOMPARE(backend->writes, 0);
    }
    void installedUnapprovedChoicesAndCaseVariantsCannotBeStaged()
    {
        auto state = FakeThemeComponentBackend::state();
        const QStringList denied{"oxygen", "Breeze_Snow", "Fusion"};
        for (int i = 0; i < themeComponentCount; ++i)
            state.fields[i].choices.append(ThemeComponentChoice{denied[i], "Installed alternative"});
        session->refresh(); backend->report(state);
        for (int i = 0; i < themeComponentCount; ++i) {
            const auto component = static_cast<ThemeComponent>(i);
            session->selectComponent(component, denied[i]);
            QCOMPARE(session->stagedValue(component), state.fields[i].value);
        }
        state.fields[2].choices.append(ThemeComponentChoice{"Kvantum", "Case variant"});
        session->refresh(); backend->report(state);
        session->selectComponent(ThemeComponent::ApplicationStyle, "Kvantum");
        QCOMPARE(session->stagedValue(ThemeComponent::ApplicationStyle), "Breeze");
        session->selectComponent(static_cast<ThemeComponent>(-1), "Windows 7 Aero");
        session->selectComponent(static_cast<ThemeComponent>(99), "Windows 7 Aero");
        QVERIFY(!session->isDirty());
        QCOMPARE(backend->writes, 0);
    }
    void applyRequiresPreflightCompletionAndAuthoritativeReadback()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectComponent(ThemeComponent::Icons, "Windows 7 Aero");
        session->selectComponent(ThemeComponent::ApplicationStyle, "kvantum");
        session->apply();
        QVERIFY(session->isBusy());
        QCOMPARE(backend->writes, 0);
        backend->report();
        QCOMPARE(backend->writes, 1);
        QCOMPARE(backend->expected, (QStringList{"breeze", "default", "Breeze"}));
        QCOMPARE(backend->requested, (QStringList{"Windows 7 Aero", "default", "kvantum"}));
        session->selectComponent(ThemeComponent::Cursors, "aero-drop");
        session->revert();
        QCOMPARE(session->stagedValue(ThemeComponent::Cursors), "default");
        backend->written();
        QVERIFY(finished.isEmpty());
        backend->report(FakeThemeComponentBackend::state(backend->requested));
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished[0][0].toBool());
        QVERIFY(!session->isDirty());
        QVERIFY(!session->hasConflict());
        QVERIFY(session->resultText().contains("readback"));
        QVERIFY(session->resultText().contains("requested"));
        QVERIFY(!session->resultText().contains("live"));
    }
    void unstagedSiblingChangeAbortsWholeApply()
    {
        session->selectComponent(ThemeComponent::Icons, "Windows 7 Aero");
        session->apply();
        backend->report(FakeThemeComponentBackend::state({"breeze", "aero-drop", "Breeze"}));
        QCOMPARE(backend->writes, 0);
        QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedValue(ThemeComponent::Icons), "Windows 7 Aero");
        QCOMPARE(session->appliedValue(ThemeComponent::Cursors), "aero-drop");
        session->revert();
        backend->report(FakeThemeComponentBackend::state({"breeze", "aero-drop", "Breeze"}));
        QVERIFY(!session->isDirty());
        QVERIFY(!session->hasConflict());
    }
    void unavailableFieldIsDisabledWithoutBlockingUnchangedSiblings()
    {
        auto state = FakeThemeComponentBackend::state();
        state.fields[1].writable = false;
        state.fields[1].unavailableReason = "Cursor policy lock";
        session->refresh(); backend->report(state);
        QVERIFY(!session->field(ThemeComponent::Cursors).canApply());
        QVERIFY(session->field(ThemeComponent::Cursors).unavailableReason.contains("policy"));
        session->selectComponent(ThemeComponent::Cursors, "aero-drop");
        QCOMPARE(session->stagedValue(ThemeComponent::Cursors), "default");
        session->selectComponent(ThemeComponent::Icons, "Windows 7 Aero");
        QVERIFY(session->canApply());
        session->apply(); backend->report(state);
        QCOMPARE(backend->writes, 1);
    }
    void capabilityOrInstalledChoiceLossDuringPreflightNeverWrites()
    {
        session->selectComponent(ThemeComponent::Icons, "Windows 7 Aero");
        session->apply();
        auto state = FakeThemeComponentBackend::state();
        state.fields[0].choices.removeLast();
        backend->report(state);
        QCOMPARE(backend->writes, 0);
        QVERIFY(session->hasConflict());
        QVERIFY(!session->canApply());
        QCOMPARE(session->stagedValue(ThemeComponent::Icons), "Windows 7 Aero");
    }
    void unknownActiveComponentsAreRetainedWithoutCoercion()
    {
        session->refresh();
        backend->report(FakeThemeComponentBackend::state({"custom-uninstalled", "alias", "CustomStyle"}));
        QCOMPARE(session->stagedValue(ThemeComponent::Icons), "custom-uninstalled");
        QCOMPARE(session->stagedValue(ThemeComponent::Cursors), "alias");
        QCOMPARE(session->stagedValue(ThemeComponent::ApplicationStyle), "CustomStyle");
        QVERIFY(!session->isDirty());
        QCOMPARE(backend->writes, 0);
    }
    void failedNotificationWithSavedReadbackRequiresExplicitRevert()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectComponent(ThemeComponent::Icons, "Windows 7 Aero");
        session->apply(); backend->report(); backend->written(false);
        backend->report(FakeThemeComponentBackend::state({"Windows 7 Aero", "default", "Breeze"}));
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(!session->isDirty());
        QVERIFY(session->hasConflict());
        QVERIFY(session->errorText().contains("Reload failed"));
        // File-watcher refresh must not erase an uncertain Apply result.
        backend->changed(); backend->report(FakeThemeComponentBackend::state({"Windows 7 Aero", "default", "Breeze"}));
        QVERIFY(session->hasConflict());
        QVERIFY(!session->errorText().isEmpty());
        session->revert(); backend->report(FakeThemeComponentBackend::state({"Windows 7 Aero", "default", "Breeze"}));
        QVERIFY(!session->hasConflict());
    }
    void partialSaveRemainsDirtyAndFailedReadbackCannotSucceed()
    {
        QSignalSpy finished(session, &SettingsSession::applyFinished);
        session->selectComponent(ThemeComponent::Icons, "Windows 7 Aero");
        session->selectComponent(ThemeComponent::ApplicationStyle, "kvantum");
        session->apply(); backend->report(); backend->written(false);
        backend->report(FakeThemeComponentBackend::state({"Windows 7 Aero", "default", "Breeze"}));
        QVERIFY(session->isDirty());
        QVERIFY(session->hasConflict());
        QCOMPARE(session->stagedValue(ThemeComponent::ApplicationStyle), "kvantum");
        session->revert(); backend->report();
        session->selectComponent(ThemeComponent::Icons, "Windows 7 Aero");
        session->apply(); backend->report(); backend->written(); backend->failRead();
        QCOMPARE(finished.size(), 2);
        QVERIFY(!finished[1][0].toBool());
        QVERIFY(!session->isReady());
        QVERIFY(session->hasConflict());
    }
    void deferredExternalRefreshPreservesReadbackFailure()
    {
        session->selectComponent(ThemeComponent::Icons, "Windows 7 Aero");
        session->apply(); backend->report();
        backend->changed();
        backend->written(); backend->report(); // helper success, persisted mismatch
        QVERIFY(session->hasConflict());
        const int reads = backend->reads;
        QTRY_COMPARE(backend->reads, reads + 1);
        backend->report();
        QVERIFY(session->hasConflict());
        QVERIFY(!session->errorText().isEmpty());
    }
private:
    FakeThemeComponentBackend *backend = nullptr;
    ThemeComponentSettings *session = nullptr;
};
QTEST_MAIN(ThemeComponentSettingsTest)
#include "theme_component_settings_test.moc"
