#include <QtTest>
#include <QDBusVariant>
#include <QDBusMetaType>
#include <QProcess>
#include <QTemporaryDir>
#include <QSizeF>
#include "platform/input/InputBackend.h"


QDBusArgument &operator<<(QDBusArgument &argument, const QSizeF &size)
{ argument.beginStructure(); argument << size.width() << size.height(); argument.endStructure(); return argument; }
const QDBusArgument &operator>>(const QDBusArgument &argument, QSizeF &size)
{ double width, height; argument.beginStructure(); argument >> width >> height; argument.endStructure(); size = QSizeF(width, height); return argument; }
class WireKWin final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin")
public slots:
    QString supportInformation() const { return QStringLiteral("KWin version: 6.6.6\n"); }
};
class WireManager final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin.InputDeviceManager")
    Q_PROPERTY(QStringList devicesSysNames READ names)
public:
    QStringList names() const { return {QStringLiteral("event6")}; }
};
class WireDevice final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin.InputDevice")
    Q_PROPERTY(QString name READ name)
    Q_PROPERTY(QString sysName READ sysName)
    Q_PROPERTY(uint vendor READ vendor)
    Q_PROPERTY(uint product READ product)
    Q_PROPERTY(bool pointer READ yes)
    Q_PROPERTY(bool touchpad READ no)
    Q_PROPERTY(bool isVirtual READ no)
    Q_PROPERTY(bool supportsLeftHanded READ yes)
    Q_PROPERTY(bool leftHanded READ handed WRITE setHanded)
    Q_PROPERTY(QSizeF size READ size)
public:
    bool left = false; int sets = 0;
    QString name() const { return QStringLiteral("Wire Mouse"); }
    QString sysName() const { return QStringLiteral("event6"); }
    uint vendor() const { return 1; }
    uint product() const { return 2; }
    bool yes() const { return true; }
    bool no() const { return false; }
    bool handed() const { return left; }
    void setHanded(bool value) { ++sets; left = value; }
    QSizeF size() const { return QSizeF(10, 20); }
};

class FakeInputTransport final : public InputTransport {
public:
    QString owner = ":1.42", version = "6.6.6";
    QVariantMap device{{"name", "Fake Mouse"}, {"sysName", "event9"}, {"vendor", 2u}, {"product", 3u},
        {"pointer", true}, {"touchpad", false}, {"isVirtual", false},
        {"enabled", true}, {"supportsDisableEvents", true}, {"enabledByDefault", true},
        {"leftHanded", false}, {"supportsLeftHanded", true}, {"leftHandedEnabledByDefault", false},
        {"pointerAcceleration", 0.0}, {"supportsPointerAcceleration", true}, {"defaultPointerAcceleration", 0.0},
        {"pointerAccelerationProfileFlat", false}, {"pointerAccelerationProfileAdaptive", true},
        {"supportsPointerAccelerationProfileFlat", true}, {"supportsPointerAccelerationProfileAdaptive", true},
        {"scrollFactor", 1.0}, {"naturalScroll", false}, {"supportsNaturalScroll", true},
        {"unrelatedVendorProperty", "preserve me"}};
    int sets = 0, maps = 0;
    int failSet = 0, externalAtMap = 0;
    bool uncertain = false, missingWritable = false, invalidNames = false, vanish = false;
    QStringList setKeys, destinations;
    int stallAtMap = 0;
    Completion stalled;
    InputReply stalledReply;
    void call(const QString &destination, const QString &, const QString &, const QString &method,
              const QVariantList &args, Completion completion) override {
        InputReply reply; reply.success = true;
        if (method == "GetNameOwner") reply.value = owner;
        else if (method == "supportInformation") reply.value = "KWin version: " + version + "\n";
        else if (method == "Get") reply.value = invalidNames ? QStringList{"../../bad"} : vanish ? QStringList{} : QStringList{"event9"};
        else if (method == "GetAll") {
            ++maps; if (externalAtMap == maps) device["naturalScroll"] = true;
            reply.value = device;
        } else if (method == "Introspect") {
            reply.value = QString("<node><interface name=\"org.kde.KWin.InputDevice\">")
                + (missingWritable ? QString() : QString("<property name=\"leftHanded\" type=\"b\" access=\"readwrite\"/>"))
                + "<property name=\"pointerAcceleration\" type=\"d\" access=\"readwrite\"/>"
                + "<property name=\"pointerAccelerationProfileFlat\" type=\"b\" access=\"readwrite\"/>"
                + "<property name=\"pointerAccelerationProfileAdaptive\" type=\"b\" access=\"readwrite\"/>"
                + "<property name=\"naturalScroll\" type=\"b\" access=\"readwrite\"/>"
                + "<property name=\"scrollFactor\" type=\"d\" access=\"readwrite\"/>"
                + "<property name=\"enabled\" type=\"b\" access=\"readwrite\"/>"
                + "</interface></node>";
        } else if (method == "Set") {
            ++sets; destinations << destination;
            const QString key = args.value(1).toString(); setKeys << key;
            if (sets == failSet) { reply.success = false; reply.errorName = uncertain ? "org.freedesktop.DBus.Error.NoReply" : "org.freedesktop.DBus.Error.AccessDenied"; }
            else {
                const QVariant value = qvariant_cast<QDBusVariant>(args.value(2)).variant();
                if (key == "pointerAccelerationProfileFlat") { device[key] = value; device["pointerAccelerationProfileAdaptive"] = !value.toBool(); }
                else if (key == "pointerAccelerationProfileAdaptive") { device[key] = value; device["pointerAccelerationProfileFlat"] = !value.toBool(); }
                else device[key] = value;
            }
        } else { reply.success = false; reply.errorName = "unexpected method"; }
        if (method == "GetAll" && maps == stallAtMap) { stalled = std::move(completion); stalledReply = reply; return; }
        QTimer::singleShot(0, this, [completion = std::move(completion), reply] { completion(reply); });
    }
};
static InputSnapshot read(KWinInputBackend &backend)
{
    QSignalSpy spy(&backend, &InputBackend::readFinished); backend.readCurrent(7);
    if (!spy.wait(3000) || spy.isEmpty() || !spy[0][1].toBool()) return {};
    return qvariant_cast<InputSnapshot>(spy[0][2]);
}
class InputBackendTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<InputSnapshot>(); qRegisterMetaType<InputOutcome>(); }
    void realPrivateBusVariantsAndOpaqueUnrelatedFields()
    {
        qDBusRegisterMetaType<QSizeF>();
        QTemporaryDir dir; QVERIFY(dir.isValid()); QProcess daemon;
        daemon.start("dbus-daemon", {"--session", "--nofork", "--print-address=1", "--address=unix:path=" + dir.path() + "/bus"});
        QVERIFY(daemon.waitForStarted(2000)); QVERIFY(daemon.waitForReadyRead(2000));
        const QString address = QString::fromUtf8(daemon.readLine()).trimmed();
        auto server = QDBusConnection::connectToBus(address, "input-wire-server");
        auto client = QDBusConnection::connectToBus(address, "input-wire-client");
        QVERIFY(server.isConnected()); QVERIFY(client.isConnected()); QVERIFY(server.registerService("org.kde.KWin"));
        WireKWin kwin; WireManager manager; WireDevice device;
        const auto flags = QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties;
        QVERIFY(server.registerObject("/KWin", &kwin, flags));
        QVERIFY(server.registerObject("/org/kde/KWin/InputDevice", &manager, flags));
        QVERIFY(server.registerObject("/org/kde/KWin/InputDevice/event6", &device, flags));
        {
            KWinInputBackend backend(client, true);
            const auto before = read(backend); QVERIFY(before.available); QVERIFY(before.writable); QCOMPARE(before.devices.size(), 1);
            QVERIFY(!before.devices[0].properties.contains("size"));
            QVERIFY(InputTypes::sameSnapshot(before, read(backend)));
            auto after = before; after.devices[0].properties["leftHanded"] = true;
            QSignalSpy done(&backend, &InputBackend::writeFinished); backend.writeChanges(1, before, after);
            QVERIFY(done.wait(3000)); QCOMPARE(qvariant_cast<InputOutcome>(done[0][1]), InputOutcome::Success);
            QCOMPARE(device.sets, 1); QVERIFY(device.left);
        }
        server.unregisterService("org.kde.KWin");
        QDBusConnection::disconnectFromBus("input-wire-client"); QDBusConnection::disconnectFromBus("input-wire-server");
        daemon.terminate(); if (!daemon.waitForFinished(1000)) { daemon.kill(); daemon.waitForFinished(1000); }
    }
    void typedWritesPreserveOtherValuesAndReadBack()
    {
        auto *bus = new FakeInputTransport; KWinInputBackend backend(bus, true);
        auto before = read(backend); QVERIFY(before.available); QVERIFY(before.writable);
        auto after = before; after.devices[0].properties["leftHanded"] = true; after.devices[0].properties["pointerAcceleration"] = 0.4;
        QSignalSpy done(&backend, &InputBackend::writeFinished); backend.writeChanges(10, before, after);
        QVERIFY(done.wait(3000)); QCOMPARE(qvariant_cast<InputOutcome>(done[0][1]), InputOutcome::Success);
        QCOMPARE(bus->sets, 2); QCOMPARE(bus->device["unrelatedVendorProperty"].toString(), "preserve me");
        QCOMPARE(bus->destinations, QStringList({":1.42", ":1.42"}));
        QCOMPARE(qvariant_cast<InputSnapshot>(done[0][2]).devices[0].properties["pointerAcceleration"].toDouble(), 0.4);
    }
    void coupledModesRequireOneConsistentWrite()
    {
        auto *bus = new FakeInputTransport; KWinInputBackend backend(bus, true);
        auto before = read(backend), after = before;
        after.devices[0].properties["pointerAccelerationProfileFlat"] = true;
        after.devices[0].properties["pointerAccelerationProfileAdaptive"] = false;
        QSignalSpy done(&backend, &InputBackend::writeFinished); backend.writeChanges(1, before, after);
        QVERIFY(done.wait(3000)); QCOMPARE(qvariant_cast<InputOutcome>(done[0][1]), InputOutcome::Success);
        QCOMPARE(bus->setKeys, QStringList{"pointerAccelerationProfileFlat"});
    }
    void externalEditBeforeWriteStopsWithoutRollback()
    {
        auto *bus = new FakeInputTransport; KWinInputBackend backend(bus, true);
        auto before = read(backend), after = before; after.devices[0].properties["leftHanded"] = true;
        bus->externalAtMap = bus->maps + 1;
        QSignalSpy done(&backend, &InputBackend::writeFinished); backend.writeChanges(1, before, after);
        QVERIFY(done.wait(3000)); QCOMPARE(bus->sets, 0); QCOMPARE(qvariant_cast<InputOutcome>(done[0][1]), InputOutcome::Failed);
        QVERIFY(bus->device["naturalScroll"].toBool());
    }
    void partialFailureKeepsConfirmedState()
    {
        auto *bus = new FakeInputTransport; KWinInputBackend backend(bus, true);
        auto before = read(backend), after = before;
        after.devices[0].properties["leftHanded"] = true; after.devices[0].properties["pointerAcceleration"] = 0.3;
        bus->failSet = 2;
        QSignalSpy done(&backend, &InputBackend::writeFinished); backend.writeChanges(1, before, after);
        QVERIFY(done.wait(3000)); QCOMPARE(qvariant_cast<InputOutcome>(done[0][1]), InputOutcome::Partial);
        QCOMPARE(bus->sets, 2); QVERIFY(bus->device["leftHanded"].toBool()); QCOMPARE(bus->device["pointerAcceleration"].toDouble(), 0.0);
    }
    void timeoutDisablesWritesUntilOwnerReplacement()
    {
        auto *bus = new FakeInputTransport; KWinInputBackend backend(bus, true);
        auto before = read(backend), after = before; after.devices[0].properties["leftHanded"] = true;
        bus->failSet = 1; bus->uncertain = true;
        QSignalSpy done(&backend, &InputBackend::writeFinished); backend.writeChanges(1, before, after);
        QVERIFY(done.wait(3000)); QCOMPARE(qvariant_cast<InputOutcome>(done[0][1]), InputOutcome::Uncertain);
        auto current = read(backend); QVERIFY(!current.writable);
        backend.writeChanges(2, before, after); QCOMPARE(bus->sets, 1);
        bus->owner = ":1.100"; current = read(backend); QVERIFY(current.writable);
    }
    void timeoutRecoveryDeadlineRetainsUncertainty_data()
    {
        QTest::addColumn<int>("failedWrite");
        QTest::newRow("first-write") << 1;
        QTest::newRow("after-verified-write") << 2;
    }
    void timeoutRecoveryDeadlineRetainsUncertainty()
    {
        QFETCH(int, failedWrite);
        auto *bus = new FakeInputTransport;
        KWinInputBackend backend(bus, true, nullptr, 1000, 50);
        auto before = read(backend), after = before;
        after.devices[0].properties["leftHanded"] = true;
        if (failedWrite == 2) after.devices[0].properties["pointerAcceleration"] = 0.3;
        bus->failSet = failedWrite; bus->uncertain = true;
        bus->stallAtMap = bus->maps + failedWrite + 1;
        QSignalSpy done(&backend, &InputBackend::writeFinished);
        backend.writeChanges(1, before, after);
        QVERIFY(done.wait(500));
        QCOMPARE(qvariant_cast<InputOutcome>(done[0][1]), InputOutcome::Uncertain);
        QCOMPARE(bus->sets, failedWrite); QVERIFY(bool(bus->stalled));
        // The late recovery reply cannot downgrade uncertainty or complete twice.
        bus->stalled(bus->stalledReply); QTest::qWait(50); QCOMPARE(done.size(), 1);
        QVERIFY(!read(backend).writable);
        backend.writeChanges(2, before, after); QCOMPARE(bus->sets, failedWrite);
    }
    void refusesUnknownReadOnlyUnsupportedAndWrongVersion()
    {
        auto *bus = new FakeInputTransport; bus->missingWritable = true; KWinInputBackend backend(bus, true);
        auto before = read(backend), after = before; QVERIFY(before.available);
        after.devices[0].properties["leftHanded"] = true;
        QSignalSpy done(&backend, &InputBackend::writeFinished); backend.writeChanges(1, before, after);
        QCOMPARE(done.size(), 1); QCOMPARE(bus->sets, 0);
        after = before; after.devices[0].properties["unrelatedVendorProperty"] = "replace"; backend.writeChanges(2, before, after); QCOMPARE(bus->sets, 0);
        bus->version = "6.7.0"; QVERIFY(!read(backend).writable);
    }
    void totalReadDeadlineIgnoresLateCompletion()
    {
        auto *bus = new FakeInputTransport; bus->stallAtMap = 1;
        KWinInputBackend backend(bus, true, nullptr, 30, 200);
        QSignalSpy done(&backend, &InputBackend::readFinished); backend.readCurrent(1);
        QVERIFY(done.wait(500)); QVERIFY(!done[0][1].toBool()); QCOMPARE(done.size(), 1);
        QVERIFY(bool(bus->stalled)); bus->stalled(bus->stalledReply);
        QTest::qWait(50); QCOMPARE(done.size(), 1); QCOMPARE(bus->sets, 0);
    }
    void failedReadbackAfterAcknowledgedSetLatchesOwner()
    {
        auto *bus = new FakeInputTransport; KWinInputBackend backend(bus, true, nullptr, 50, 1000);
        auto before = read(backend), after = before; after.devices[0].properties["leftHanded"] = true;
        bus->stallAtMap = bus->maps + 2;
        QSignalSpy done(&backend, &InputBackend::writeFinished); backend.writeChanges(1, before, after);
        QVERIFY(done.wait(1000)); QCOMPARE(qvariant_cast<InputOutcome>(done[0][1]), InputOutcome::Uncertain);
        QCOMPARE(bus->sets, 1); QVERIFY(!read(backend).writable);
        bus->stalled(bus->stalledReply); QTest::qWait(50); QCOMPARE(done.size(), 1); QCOMPARE(bus->sets, 1);
    }
    void rejectsInvalidInventoryAndOwnerChange()
    {
        auto *bus = new FakeInputTransport; KWinInputBackend backend(bus, true);
        auto before = read(backend), after = before; after.devices[0].properties["leftHanded"] = true;
        bus->owner = ":1.50";
        QSignalSpy done(&backend, &InputBackend::writeFinished); backend.writeChanges(1, before, after);
        QVERIFY(done.wait(3000)); QCOMPARE(bus->sets, 0);
        bus->invalidNames = true; QVERIFY(!read(backend).available);
    }
};
QTEST_GUILESS_MAIN(InputBackendTest)
#include "input_backend_test.moc"
