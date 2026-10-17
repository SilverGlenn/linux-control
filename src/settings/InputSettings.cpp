#include "InputSettings.h"
#include "platform/input/InputBackend.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <cmath>

const InputDevice *InputSnapshot::device(const QString &id) const
{ for (const auto &d : devices) if (d.id == id) return &d; return nullptr; }
namespace InputTypes {
const QList<InputField> &fields()
{
    static const QList<InputField> list{
        {"enabled", "Enable this device", "Device", "supportsDisableEvents", "enabledByDefault", InputField::Boolean},
        {"disableEventsOnExternalMouse", "Disable touchpad when a mouse is connected", "Device", "supportsDisableEventsOnExternalMouse", "disableEventsOnExternalMouseEnabledByDefault", InputField::Boolean, 0, 1, true},
        {"disableWhileTyping", "Disable touchpad while typing", "Device", "supportsDisableWhileTyping", "disableWhileTypingEnabledByDefault", InputField::Boolean, 0, 1, true},
        {"leftHanded", "Swap primary and secondary buttons", "Buttons", "supportsLeftHanded", "leftHandedEnabledByDefault", InputField::Boolean},
        {"middleEmulation", "Press both buttons for a middle click", "Buttons", "supportsMiddleEmulation", "middleEmulationEnabledByDefault", InputField::Boolean},
        {"clickMethodAreas", "Use button areas for touchpad clicks", "Buttons", "supportsClickMethodAreas", "defaultClickMethodAreas", InputField::Boolean, 0, 1, true},
        {"clickMethodClickfinger", "Use finger count for touchpad clicks", "Buttons", "supportsClickMethodClickfinger", "defaultClickMethodClickfinger", InputField::Boolean, 0, 1, true},
        {"pointerAcceleration", "Pointer speed", "Pointer Options", "supportsPointerAcceleration", "defaultPointerAcceleration", InputField::Real, -1, 1},
        {"pointerAccelerationProfileFlat", "Constant pointer speed", "Pointer Options", "supportsPointerAccelerationProfileFlat", "defaultPointerAccelerationProfileFlat", InputField::Boolean},
        {"pointerAccelerationProfileAdaptive", "Adaptive pointer acceleration", "Pointer Options", "supportsPointerAccelerationProfileAdaptive", "defaultPointerAccelerationProfileAdaptive", InputField::Boolean},
        {"naturalScroll", "Reverse scrolling direction", "Scrolling", "supportsNaturalScroll", "naturalScrollEnabledByDefault", InputField::Boolean},
        {"scrollTwoFinger", "Scroll with two fingers", "Scrolling", "supportsScrollTwoFinger", "scrollTwoFingerEnabledByDefault", InputField::Boolean, 0, 1, true},
        {"scrollEdge", "Scroll along the touchpad edge", "Scrolling", "supportsScrollEdge", "scrollEdgeEnabledByDefault", InputField::Boolean, 0, 1, true},
        {"scrollOnButtonDown", "Scroll while holding a button", "Scrolling", "supportsScrollOnButtonDown", "scrollOnButtonDownEnabledByDefault", InputField::Boolean},
        {"scrollButton", "Button used for scrolling", "Scrolling", "supportsScrollOnButtonDown", "defaultScrollButton", InputField::Button, 0, 0xffffffffu},
        {"scrollFactor", "Scrolling speed multiplier", "Scrolling", "", "", InputField::Real, 0.1, 10},
        {"tapToClick", "Tap to click", "Tapping", "tapFingerCount", "tapToClickEnabledByDefault", InputField::Boolean, 0, 1, true},
        {"lmrTapButtonMap", "Two fingers: middle click; three fingers: secondary click", "Tapping", "supportsLmrTapButtonMap", "lmrTapButtonMapEnabledByDefault", InputField::Boolean, 0, 1, true},
        {"tapAndDrag", "Tap, then drag", "Tapping", "tapFingerCount", "tapAndDragEnabledByDefault", InputField::Boolean, 0, 1, true},
        {"tapDragLock", "Keep dragging after lifting your fingers", "Tapping", "tapFingerCount", "tapDragLockEnabledByDefault", InputField::Boolean, 0, 1, true},
    };
    return list;
}
const InputField *field(const QString &key)
{ for (const auto &f : fields()) if (key == QLatin1String(f.key)) return &f; return nullptr; }
bool supported(const InputDevice &device, const InputField &f)
{
    const auto &p = device.properties;
    if (!p.value(QStringLiteral("__writable")).toStringList().contains(QLatin1String(f.key))) return false;
    if (!p.value(QStringLiteral("pointer")).toBool() || p.value(QStringLiteral("isVirtual")).toBool()
        || (f.touchpadOnly && !device.isTouchpad()) || !p.contains(QLatin1String(f.key))) return false;
    if (*f.support) {
        const auto v = p.value(QLatin1String(f.support));
        if (QLatin1String(f.support) == QLatin1String("tapFingerCount")) {
            if (v.metaType().id() != QMetaType::Int || v.toInt() <= 0) return false;
        } else if (v.metaType().id() != QMetaType::Bool || !v.toBool()) return false;
    }
    if (f.type == InputField::Button) {
        const auto mask = p.value(QStringLiteral("supportedButtons"));
        if (mask.metaType().id() != QMetaType::Int || mask.toInt() < 0) return false;
    }
    const int type = p.value(QLatin1String(f.key)).metaType().id();
    return f.type == InputField::Boolean ? type == QMetaType::Bool
        : f.type == InputField::Real ? type == QMetaType::Double
        : type == QMetaType::UInt;
}
QStringList coupledProperties(const QString &key)
{
    if (key.startsWith(QLatin1String("pointerAccelerationProfile")))
        return {QStringLiteral("pointerAccelerationProfileFlat"), QStringLiteral("pointerAccelerationProfileAdaptive")};
    if (key.startsWith(QLatin1String("clickMethod")))
        return {QStringLiteral("clickMethodAreas"), QStringLiteral("clickMethodClickfinger")};
    if (key == QLatin1String("scrollTwoFinger") || key == QLatin1String("scrollEdge") || key == QLatin1String("scrollOnButtonDown"))
        return {QStringLiteral("scrollTwoFinger"), QStringLiteral("scrollEdge"), QStringLiteral("scrollOnButtonDown")};
    return {};
}
bool validate(const InputDevice &device, const QString &key, const QVariant &value, QString &error)
{
    const auto *f = field(key);
    if (!f || !supported(device, *f)) { error = QObject::tr("This device does not support the selected setting."); return false; }
    const int type = value.metaType().id();
    if (f->type == InputField::Boolean) {
        if (type != QMetaType::Bool) { error = QObject::tr("A true or false value is required."); return false; }
        const auto group = coupledProperties(key);
        if (group.size() == 2 && !value.toBool()) {
            const QString peer = group[0] == key ? group[1] : group[0];
            const auto *other = field(peer);
            if (!other || !supported(device, *other)) { error = QObject::tr("The alternate mode is not supported by this device."); return false; }
        }
    } else if (f->type == InputField::Real) {
        if (type != QMetaType::Double || !std::isfinite(value.toDouble()) || value.toDouble() < f->minimum || value.toDouble() > f->maximum) {
            error = QObject::tr("The value is outside the supported range."); return false;
        }
        if (key == QLatin1String("pointerAcceleration") && std::abs(value.toDouble() * 1000 - std::round(value.toDouble() * 1000)) > 0.000001) {
            error = QObject::tr("Pointer speed supports at most three decimal places."); return false;
        }
    } else {
        if (type != QMetaType::UInt) { error = QObject::tr("A supported mouse button is required."); return false; }
        const uint button = value.toUInt();
        // Linux input-event button codes, translated from KWin's Qt button mask.
        const uint mask = device.properties.value(QStringLiteral("supportedButtons")).toUInt();
        const uint bit = button == 272 ? 1u : button == 273 ? 2u : button == 274 ? 4u : button >= 275 && button <= 287 ? (8u << (button - 275)) : 0u;
        if (button != 0 && (!bit || !(mask & bit))) { error = QObject::tr("That button is not advertised by this device."); return false; }
    }
    return true;
}
void setPreference(InputDevice &device, const QString &key, const QVariant &value)
{
    const auto group = coupledProperties(key);
    if (!group.isEmpty()) {
        if (value.toBool()) {
            for (const auto &peer : group) if (device.properties.contains(peer)) device.properties[peer] = peer == key;
        } else if (group.size() == 2) {
            for (const auto &peer : group) if (device.properties.contains(peer)) device.properties[peer] = peer != key;
        } else {
            device.properties[key] = false;
        }
    } else device.properties[key] = value;
}
bool sameDevice(const InputDevice &a, const InputDevice &b)
{
    if (a.id != b.id || a.objectPath != b.objectPath || a.name != b.name) return false;
    for (const auto *key : {"sysName", "vendor", "product", "pointer", "touchpad", "isVirtual"})
        if (a.properties.value(QLatin1String(key)) != b.properties.value(QLatin1String(key))) return false;
    return true;
}
bool sameSnapshot(const InputSnapshot &a, const InputSnapshot &b)
{
    if (a.owner != b.owner || a.version != b.version || a.topologyEpoch != b.topologyEpoch || a.devices.size() != b.devices.size()) return false;
    for (const auto &d : a.devices) {
        const auto *other = b.device(d.id);
        if (!other || !sameDevice(d, *other) || d.properties != other->properties) return false;
    }
    return true;
}
}
InputSettings *InputSettings::instance()
{
    static QPointer<InputSettings> session;
    if (!session) session = new InputSettings(new KWinInputBackend, QCoreApplication::instance());
    return session;
}
InputSettings::InputSettings(InputBackend *backend, QObject *parent) : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend); backend->setParent(this);
    connect(backend, &InputBackend::readFinished, this, &InputSettings::onRead);
    connect(backend, &InputBackend::writeFinished, this, &InputSettings::onWritten);
    connect(backend, &InputBackend::externalChange, this, &InputSettings::refresh);
    m_busy = true; QTimer::singleShot(0, this, &InputSettings::read);
}
void InputSettings::selectProperty(const QString &id, const QString &key, const QVariant &value)
{
    if (m_busy || !m_ready || m_uncertain || m_conflict) return;
    for (auto &d : m_staged.devices) if (d.id == id) {
        QString error;
        if (!InputTypes::validate(d, key, value, error)) { m_error = error; emit stateChanged(); return; }
        InputTypes::setPreference(d, key, value);
        m_dirty = !InputTypes::sameSnapshot(m_applied, m_staged);
        m_error.clear(); m_result.clear(); emit stateChanged(); return;
    }
}
void InputSettings::restoreDeviceDefaults(const QString &id)
{
    if (m_busy || !m_ready || m_uncertain || m_conflict) return;
    const auto *d = m_applied.device(id);
    if (!d) return;
    // Stage only exported defaults. Do not invent ScrollFactor's config-driven default.
    const InputDevice baseline = *d;
    for (const auto &f : InputTypes::fields()) {
        if (!*f.defaultKey || !InputTypes::supported(baseline, f) || !baseline.properties.contains(QLatin1String(f.defaultKey))) continue;
        const auto value = baseline.properties.value(QLatin1String(f.defaultKey));
        if (!InputTypes::coupledProperties(QLatin1String(f.key)).isEmpty() && !value.toBool()) continue;
        selectProperty(id, QLatin1String(f.key), value);
    }
    // A scroll method may legitimately default to none.
    bool anyDefault = false, hasScrollDefault = false;
    for (const auto *key : {"scrollTwoFinger", "scrollEdge", "scrollOnButtonDown"}) {
        const auto *f = InputTypes::field(QLatin1String(key));
        if (InputTypes::supported(baseline, *f) && baseline.properties.contains(QLatin1String(f->defaultKey))) {
            hasScrollDefault = true; anyDefault |= baseline.properties.value(QLatin1String(f->defaultKey)).toBool();
        }
    }
    if (hasScrollDefault && !anyDefault)
        for (const auto *key : {"scrollTwoFinger", "scrollEdge", "scrollOnButtonDown"}) {
            const auto *f = InputTypes::field(QLatin1String(key));
            if (InputTypes::supported(baseline, *f)) selectProperty(id, QLatin1String(key), false);
        }
    m_result = tr("Device-reported defaults are staged. Scrolling speed is unchanged because its default is not exported by KDE.");
    emit stateChanged();
}
void InputSettings::read()
{ m_pendingRefresh = false; m_reading = true; m_busy = true; emit stateChanged(); m_backend->readCurrent(++m_token); }
void InputSettings::refresh()
{ if (m_busy) { m_pendingRefresh = true; return; } read(); }
void InputSettings::revert()
{
    if (m_busy) return;
    m_dirty = false; m_conflict = false; m_error.clear(); m_result.clear(); read();
}
void InputSettings::apply()
{
    if (m_busy) return;
    if (!canApply()) {
        m_error = m_uncertain ? tr("An earlier input write may still complete. Further writes are disabled until KWin's owner changes.")
            : m_conflict ? tr("Input settings changed outside Control Panel. Revert to reload them.") : m_applied.reason;
        emit stateChanged(); emit applyFinished(false); return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_writing = true; m_busy = true; m_error.clear(); m_result.clear(); emit stateChanged();
    m_backend->writeChanges(++m_token, m_applied, m_staged);
}
void InputSettings::onRead(quint64 token, bool success, const InputSnapshot &snapshot, const QString &error)
{
    if (!m_reading || token != m_token) return;
    m_reading = false;
    const bool changed = m_ready && !InputTypes::sameSnapshot(m_applied, snapshot);
    if (success) {
        if (!m_uncertainOwner.isEmpty() && snapshot.owner != m_uncertainOwner) { m_uncertain = false; m_uncertainOwner.clear(); }
        m_applied = snapshot; m_ready = snapshot.available;
        if (!m_dirty) m_staged = snapshot;
        else if (changed) m_conflict = true;
        m_dirty = !InputTypes::sameSnapshot(m_applied, m_staged);
        if (m_conflict) m_error = tr("Input settings or devices changed outside Control Panel. Your choices remain staged; Revert reloads current state.");
        else if (!m_uncertain) m_error.clear();
    } else {
        m_ready = false; if (m_dirty) m_conflict = true;
        m_error = error.isEmpty() ? tr("Input devices could not be read.") : error;
    }
    m_busy = false; emit stateChanged();
    if (m_pendingRefresh) QTimer::singleShot(0, this, &InputSettings::refresh);
}
void InputSettings::onWritten(quint64 token, InputOutcome outcome, const InputSnapshot &snapshot, const QString &error)
{
    if (!m_writing || token != m_token) return;
    const QString mutationOwner = m_applied.owner;
    if (outcome == InputOutcome::Success && (!snapshot.available || !InputTypes::sameSnapshot(m_staged, snapshot)))
        outcome = InputOutcome::Uncertain;
    m_writing = false; m_outcome = outcome; m_busy = false;
    if (snapshot.available) { m_applied = snapshot; m_ready = true; }
    else m_ready = false;
    if (outcome == InputOutcome::Success) {
        m_staged = m_applied; m_dirty = false; m_conflict = false;
        m_result = tr("Input settings applied and KWin's active values verified. KWin requests saving these preferences; disk durability and physical behavior are not verified here.");
        m_error.clear();
    } else {
        m_dirty = !InputTypes::sameSnapshot(m_applied, m_staged); m_conflict = true;
        if (outcome == InputOutcome::Uncertain) {
            m_uncertainOwner = mutationOwner;
            m_uncertain = !snapshot.available || snapshot.owner == mutationOwner;
        }
        m_error = error.isEmpty() ? tr("Input settings were not fully applied. Some values may have changed. Revert reloads current state without undoing external changes.") : error;
        m_result.clear();
    }
    emit applyFinished(outcome == InputOutcome::Success); emit stateChanged();
    if (m_pendingRefresh) QTimer::singleShot(0, this, &InputSettings::refresh);
}
