#include "KeyboardSettings.h"
#include "platform/keyboard/KeyboardBackend.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QPointer>
#include <QTimer>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>

namespace {
KeyboardChoice readChoice(QXmlStreamReader &xml)
{
    KeyboardChoice result;
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("name")) result.id = xml.readElementText();
        else if (xml.name() == QLatin1String("description")) result.description = xml.readElementText();
        else xml.skipCurrentElement();
    }
    return result;
}
KeyboardLayoutInfo readLayout(QXmlStreamReader &xml)
{
    KeyboardLayoutInfo result;
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("configItem")) {
            const auto value = readChoice(xml); result.id = value.id; result.description = value.description;
        } else if (xml.name() == QLatin1String("variantList")) {
            while (xml.readNextStartElement()) {
                if (xml.name() != QLatin1String("variant")) { xml.skipCurrentElement(); continue; }
                while (xml.readNextStartElement()) {
                    if (xml.name() == QLatin1String("configItem")) result.variants << readChoice(xml);
                    else xml.skipCurrentElement();
                }
            }
        } else xml.skipCurrentElement();
    }
    return result;
}
KeyboardOptionGroup readGroup(QXmlStreamReader &xml)
{
    KeyboardOptionGroup result;
    result.allowMultiple = xml.attributes().value(QStringLiteral("allowMultipleSelection")) == QLatin1String("true");
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("configItem")) {
            const auto value = readChoice(xml); result.id = value.id; result.description = value.description;
        } else if (xml.name() == QLatin1String("option")) {
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("configItem")) result.options << readChoice(xml);
                else xml.skipCurrentElement();
            }
        } else xml.skipCurrentElement();
    }
    return result;
}
bool safeId(const QString &id)
{ return !id.isEmpty() && id.size() <= 256 && !id.contains(QChar::Null) && !id.contains(',') && !id.contains('/') && !id.contains('\n'); }
bool parseRegistry(const QByteArray &bytes, KeyboardRegistry &registry, bool extras)
{
    QXmlStreamReader xml(bytes);
    if (!xml.readNextStartElement() || xml.name() != QLatin1String("xkbConfigRegistry")) return false;
    while (xml.readNextStartElement()) {
        const QString section = xml.name().toString();
        if (section != QLatin1String("modelList") && section != QLatin1String("layoutList") && section != QLatin1String("optionList")) { xml.skipCurrentElement(); continue; }
        while (xml.readNextStartElement()) {
            if (section == QLatin1String("modelList") && xml.name() == QLatin1String("model")) {
                while (xml.readNextStartElement()) {
                    if (xml.name() == QLatin1String("configItem")) {
                        auto value = readChoice(xml);
                        if (!safeId(value.id)) return false;
                        if (std::none_of(registry.models.begin(), registry.models.end(), [&](const auto &v) { return v.id == value.id; })) registry.models << value;
                        else if (!extras) return false;
                    } else xml.skipCurrentElement();
                }
            } else if (section == QLatin1String("layoutList") && xml.name() == QLatin1String("layout")) {
                const auto value = readLayout(xml);
                if (!safeId(value.id)) return false;
                auto existing = std::find_if(registry.layouts.begin(), registry.layouts.end(), [&](const auto &v) { return v.id == value.id; });
                if (existing == registry.layouts.end()) registry.layouts << value;
                else if (extras) {
                    for (const auto &variant : value.variants)
                        if (std::none_of(existing->variants.begin(), existing->variants.end(), [&](const auto &v) { return v.id == variant.id; })) existing->variants << variant;
                } else return false;
            } else if (section == QLatin1String("optionList") && xml.name() == QLatin1String("group")) {
                const auto value = readGroup(xml);
                if (!safeId(value.id)) return false;
                auto existing = std::find_if(registry.optionGroups.begin(), registry.optionGroups.end(), [&](const auto &v) { return v.id == value.id; });
                if (existing == registry.optionGroups.end()) registry.optionGroups << value;
                else if (extras) {
                    for (const auto &option : value.options)
                        if (std::none_of(existing->options.begin(), existing->options.end(), [&](const auto &v) { return v.id == option.id; })) existing->options << option;
                } else return false;
            } else xml.skipCurrentElement();
        }
    }
    while (!xml.atEnd()) xml.readNext();
    if (xml.hasError() || registry.models.size() > 4096 || registry.layouts.size() > 4096 || registry.optionGroups.size() > 2048) return false;
    for (const auto &layout : registry.layouts) {
        QSet<QString> seen;
        for (const auto &variant : layout.variants) { if (!safeId(variant.id) || seen.contains(variant.id)) return false; seen.insert(variant.id); }
    }
    for (const auto &group : registry.optionGroups) {
        QSet<QString> seen;
        for (const auto &option : group.options) { if (!safeId(option.id) || seen.contains(option.id)) return false; seen.insert(option.id); }
    }
    return true;
}
QVariantMap canonical(QVariantMap value)
{
    for (const auto *key : {"VariantList", "DisplayNames"}) {
        if (!value.contains(QLatin1String(key))) continue;
        auto list = value.value(QLatin1String(key)).toStringList();
        while (!list.isEmpty() && list.last().isEmpty()) list.removeLast();
        value[QLatin1String(key)] = list;
    }
    return value;
}
bool sameValue(const QString &key, const QVariant &a, const QVariant &b)
{
    if (key == QLatin1String("RepeatRate")) return std::abs(a.toDouble() - b.toDouble()) < 0.000000001;
    return a == b;
}
}

const KeyboardLayoutInfo *KeyboardRegistry::layout(const QString &id) const
{ for (const auto &value : layouts) if (value.id == id) return &value; return nullptr; }

QVariantMap KeyboardTypes::defaults()
{
    return {{QStringLiteral("KeyRepeat"), qEnvironmentVariable("QT_IM_MODULE") == QLatin1String("plasmaim") ? QStringLiteral("accent") : QStringLiteral("repeat")},
        {QStringLiteral("RepeatDelay"), 600}, {QStringLiteral("RepeatRate"), 25.0}, {QStringLiteral("NumLock"), 2},
        {QStringLiteral("Model"), QStringLiteral("pc104")}, {QStringLiteral("Use"), false},
        {QStringLiteral("LayoutList"), QStringList{}}, {QStringLiteral("VariantList"), QStringList{}},
        {QStringLiteral("DisplayNames"), QStringList{}}, {QStringLiteral("Options"), QStringList{}},
        {QStringLiteral("ResetOldOptions"), false}, {QStringLiteral("SwitchMode"), QStringLiteral("Global")},
        {QStringLiteral("LayoutLoopCount"), -1}, {QStringLiteral("kbdLayoutChangedEnabled"), true}};
}

KeyboardRegistry KeyboardTypes::readRegistry(const QString &path, const QString &extrasPath)
{
    KeyboardRegistry result; QCryptographicHash hash(QCryptographicHash::Sha256);
    const QStringList paths = extrasPath.isEmpty() ? QStringList{path} : QStringList{path, extrasPath};
    for (qsizetype i = 0; i < paths.size(); ++i) {
        QFile file(paths[i]);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) {
            result.error = QStringLiteral("The installed keyboard registry could not be read."); return result;
        }
        const auto bytes = file.readAll(); hash.addData(bytes);
        if (!parseRegistry(bytes, result, i > 0)) { result.error = QStringLiteral("The keyboard registry is invalid or unsupported."); return result; }
    }
    result.revision = hash.result();
    result.valid = !result.models.isEmpty() && !result.layouts.isEmpty();
    if (!result.valid) result.error = QStringLiteral("The keyboard registry has no models or layouts.");
    return result;
}

QStringList KeyboardTypes::changedKeys(const QVariantMap &a, const QVariantMap &b)
{
    const auto x = canonical(a), y = canonical(b);
    QSet<QString> keys;
    for (auto i = x.begin(); i != x.end(); ++i) keys.insert(i.key());
    for (auto i = y.begin(); i != y.end(); ++i) keys.insert(i.key());
    QStringList changed;
    for (const auto &key : keys)
        if (!x.contains(key) || !y.contains(key) || a.value(key).metaType() != b.value(key).metaType()
            || !sameValue(key, x.value(key), y.value(key))) changed << key;
    changed.sort(); return changed;
}
bool KeyboardTypes::sameValues(const QVariantMap &a, const QVariantMap &b) { return changedKeys(a, b).isEmpty(); }

bool KeyboardTypes::validate(const KeyboardSnapshot &snapshot, const QVariantMap &requested, QString &error)
{
    error.clear(); auto fail = [&](const QString &message) { error = message; return false; };
    const auto known = defaults(); const auto changed = changedKeys(snapshot.values, requested);
    for (const auto &key : changed) {
        if (!known.contains(key) || !requested.contains(key)) return fail(QStringLiteral("Unknown keyboard fields cannot be changed."));
        if (snapshot.immutableKeys.contains(key)) return fail(QStringLiteral("This keyboard preference is locked or unavailable on this session: %1.").arg(key));
        const auto value = requested.value(key);
        if (value.metaType() != known.value(key).metaType()) return fail(QStringLiteral("The keyboard value has an invalid type: %1.").arg(key));
        if (key == QLatin1String("KeyRepeat") && !QStringList{QStringLiteral("nothing"), QStringLiteral("repeat"), QStringLiteral("accent")}.contains(value.toString())) return fail(QStringLiteral("Select a supported key-repeat behavior."));
        if (key == QLatin1String("RepeatDelay") && (value.toInt() < 100 || value.toInt() > 5000)) return fail(QStringLiteral("Repeat delay must be between 100 and 5000 milliseconds."));
        if (key == QLatin1String("RepeatRate") && (!std::isfinite(value.toDouble()) || value.toDouble() < 0.2 || value.toDouble() > 200)) return fail(QStringLiteral("Repeat rate must be between 0.2 and 200 repeats per second."));
        if (key == QLatin1String("NumLock") && (value.toInt() < 0 || value.toInt() > 2)) return fail(QStringLiteral("Select a supported startup Num Lock preference."));
        if (key == QLatin1String("SwitchMode") && !QStringList{QStringLiteral("Global"),QStringLiteral("Desktop"),QStringLiteral("WinClass"),QStringLiteral("Window")}.contains(value.toString())) return fail(QStringLiteral("Select a supported layout-switching policy."));
        if (key == QLatin1String("Model")) {
            if (!snapshot.registry.valid || std::none_of(snapshot.registry.models.begin(), snapshot.registry.models.end(), [&](const auto &m) { return m.id == value.toString(); })) return fail(QStringLiteral("Choose a model from the installed keyboard registry."));
        }
    }
    const bool layoutChanged = changed.contains(QStringLiteral("LayoutList")) || changed.contains(QStringLiteral("VariantList")) || changed.contains(QStringLiteral("DisplayNames"));
    if (layoutChanged) {
        const auto layouts = requested.value(QStringLiteral("LayoutList")).toStringList();
        const auto variants = requested.value(QStringLiteral("VariantList")).toStringList();
        const auto names = requested.value(QStringLiteral("DisplayNames")).toStringList();
        if (!snapshot.registry.valid || layouts.size() > 32 || variants.size() > layouts.size() || names.size() > layouts.size()) return fail(QStringLiteral("The keyboard layout lists are inconsistent or unsupported."));
        const auto oldLayouts = snapshot.values.value(QStringLiteral("LayoutList")).toStringList();
        const auto oldVariants = snapshot.values.value(QStringLiteral("VariantList")).toStringList();
        const auto oldNames = snapshot.values.value(QStringLiteral("DisplayNames")).toStringList();
        for (qsizetype i = 0; i < layouts.size(); ++i) {
            const auto *layout = snapshot.registry.layout(layouts[i]);
            const bool unchanged = oldLayouts.value(i) == layouts[i] && oldVariants.value(i) == variants.value(i);
            if (!unchanged && (!layout || (!variants.value(i).isEmpty() && std::none_of(layout->variants.begin(), layout->variants.end(), [&](const auto &v) { return v.id == variants.value(i); })))) return fail(QStringLiteral("Choose a layout and variant from the installed registry."));
            if (names.value(i) != oldNames.value(i) && (names.value(i).size() > 3 || names.value(i).contains(QChar::Null) || names.value(i).contains('\n'))) return fail(QStringLiteral("Layout labels support at most three characters."));
        }
    }
    if (changed.contains(QStringLiteral("Use")) || layoutChanged) {
        const bool use = requested.value(QStringLiteral("Use")).toBool();
        const auto layouts = requested.value(QStringLiteral("LayoutList")).toStringList();
        if (use && layouts.isEmpty()) return fail(QStringLiteral("Add a keyboard layout before enabling custom layouts."));
        if (!use && (!layouts.isEmpty() || !canonical(requested).value(QStringLiteral("VariantList")).toStringList().isEmpty()
            || !canonical(requested).value(QStringLiteral("DisplayNames")).toStringList().isEmpty()))
            return fail(QStringLiteral("Disabling custom layouts requires clearing the layout, variant and label lists."));
    }
    if (changed.contains(QStringLiteral("LayoutLoopCount"))) {
        const int count = requested.value(QStringLiteral("LayoutLoopCount")).toInt();
        const int size = requested.value(QStringLiteral("LayoutList")).toStringList().size();
        if (count != -1 && (count < 2 || count > std::min(4, size - 1))) return fail(QStringLiteral("A loop subset requires at least two layouts and a spare layout."));
    }
    if (changed.contains(QStringLiteral("Options"))) {
        const auto options = requested.value(QStringLiteral("Options")).toStringList();
        const auto before = snapshot.values.value(QStringLiteral("Options")).toStringList();
        if (!snapshot.registry.valid || options.size() > 256 || QSet<QString>(options.begin(), options.end()).size() != options.size()) return fail(QStringLiteral("The keyboard option list is invalid."));
        QSet<QString> available;
        for (const auto &group : snapshot.registry.optionGroups) {
            QSet<QString> selected, oldSelected;
            for (const auto &option : group.options) {
                available.insert(option.id);
                if (options.contains(option.id)) selected.insert(option.id);
                if (before.contains(option.id)) oldSelected.insert(option.id);
            }
            if (!group.allowMultiple && selected.size() > 1 && selected != oldSelected) return fail(QStringLiteral("Select only one option in this keyboard option group: %1.").arg(group.description));
        }
        for (const auto &option : options) if (!available.contains(option) && !before.contains(option)) return fail(QStringLiteral("Choose options from the installed keyboard registry."));
    }
    return true;
}

KeyboardSettings *KeyboardSettings::instance()
{
    static QPointer<KeyboardSettings> session;
    if (!session) session = new KeyboardSettings(new KdeKeyboardBackend, QCoreApplication::instance());
    return session;
}
KeyboardSettings::KeyboardSettings(KeyboardBackend *backend, QObject *parent) : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend); backend->setParent(this);
    qRegisterMetaType<KeyboardSnapshot>(); qRegisterMetaType<KeyboardOutcome>();
    connect(backend, &KeyboardBackend::readFinished, this, &KeyboardSettings::onRead);
    connect(backend, &KeyboardBackend::writeFinished, this, &KeyboardSettings::onWritten);
    connect(backend, &KeyboardBackend::externalChange, this, &KeyboardSettings::refresh);
    m_busy = true; QTimer::singleShot(0, this, &KeyboardSettings::read);
}
bool KeyboardSettings::canApply() const
{ return m_ready && m_applied.writable && !m_conflict && (m_lockedOwner.isEmpty() || m_applied.owner != m_lockedOwner); }
void KeyboardSettings::selectValue(const QString &key, const QVariant &value) { selectValues({{key,value}}); }
void KeyboardSettings::selectValues(const QVariantMap &delta)
{
    if (m_busy || !canApply()) return;
    auto values = m_staged.values;
    for (auto i = delta.begin(); i != delta.end(); ++i) values[i.key()] = i.value();
    if (delta.contains(QStringLiteral("Use")) && !delta.value(QStringLiteral("Use")).toBool()) {
        values[QStringLiteral("LayoutList")] = QStringList{};
        values[QStringLiteral("VariantList")] = QStringList{};
        values[QStringLiteral("DisplayNames")] = QStringList{};
    }
    if (delta.contains(QStringLiteral("LayoutList")) && values.value(QStringLiteral("LayoutList")).toStringList().isEmpty()) {
        values[QStringLiteral("Use")] = false;
        values[QStringLiteral("VariantList")] = QStringList{};
        values[QStringLiteral("DisplayNames")] = QStringList{};
    }
    QString error;
    if (!KeyboardTypes::validate(m_applied, values, error)) { m_error = error; emit stateChanged(); return; }
    m_staged.values = values; m_dirty = !KeyboardTypes::sameValues(m_applied.values, values);
    m_error.clear(); m_result.clear(); m_keepError = false; emit stateChanged();
}
void KeyboardSettings::restoreDefaults()
{
    QVariantMap delta;
    const auto values = KeyboardTypes::defaults();
    const QSet<QString> coupledLayouts{QStringLiteral("Use"), QStringLiteral("LayoutList"), QStringLiteral("VariantList"), QStringLiteral("DisplayNames")};
    const bool layoutGroupLocked = std::any_of(coupledLayouts.begin(), coupledLayouts.end(), [&](const auto &key) { return m_applied.immutableKeys.contains(key); });
    for (auto i = values.begin(); i != values.end(); ++i)
        if (!m_applied.immutableKeys.contains(i.key()) && !(layoutGroupLocked && coupledLayouts.contains(i.key()))) delta[i.key()] = i.value();
    selectValues(delta);
}
void KeyboardSettings::read()
{ m_reading = true; m_busy = true; m_pendingRefresh = false; emit stateChanged(); m_backend->readCurrent(++m_token); }
void KeyboardSettings::refresh() { if (m_busy) m_pendingRefresh = true; else read(); }
void KeyboardSettings::revert()
{
    if (m_busy) return;
    m_dirty = false; m_conflict = false; m_error.clear(); m_result.clear(); m_keepError = false; read();
}
void KeyboardSettings::apply()
{
    if (m_busy) return;
    QString error;
    if (!canApply() || !KeyboardTypes::validate(m_applied, m_staged.values, error)) {
        m_error = !error.isEmpty() ? error : !m_lockedOwner.isEmpty() && m_lockedOwner == m_applied.owner ? QStringLiteral("An earlier keyboard change is uncertain. Further writes remain disabled until KWin restarts.")
            : m_conflict ? QStringLiteral("Keyboard preferences changed outside Control Panel. Revert to reload them.") : m_applied.reason;
        emit stateChanged(); emit applyFinished(false); return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_requested = m_staged.values; m_writeOwner = m_applied.owner; m_writeVersion = m_applied.version; m_writing = m_busy = true;
    m_error.clear(); m_result.clear(); emit stateChanged(); m_backend->writePreferences(++m_token,m_applied,m_requested);
}
void KeyboardSettings::onRead(quint64 token, bool success, const KeyboardSnapshot &snapshot, const QString &error)
{
    if (!m_reading || token != m_token) return;
    m_reading = false;
    if (success) {
        const bool changed = m_ready && (m_applied.revision != snapshot.revision || m_applied.owner != snapshot.owner || m_applied.version != snapshot.version
            || m_applied.immutableKeys != snapshot.immutableKeys || m_applied.registry.revision != snapshot.registry.revision
            || m_applied.writable != snapshot.writable || m_applied.reloadAvailable != snapshot.reloadAvailable
            || !KeyboardTypes::sameValues(m_applied.values,snapshot.values));
        if (!snapshot.owner.isEmpty() && snapshot.owner != m_lockedOwner) m_lockedOwner.clear();
        m_applied = snapshot; m_ready = snapshot.available;
        if (!m_dirty) m_staged = snapshot;
        else if (changed) { m_conflict = true; m_error = QStringLiteral("Keyboard preferences changed outside Control Panel. Your choices remain staged; Revert reloads them."); }
        if (!m_conflict && !m_keepError && m_lockedOwner.isEmpty()) m_error.clear();
    } else { m_ready = false; if (m_dirty) m_conflict = true; m_error = error.isEmpty() ? QStringLiteral("Keyboard preferences could not be read.") : error; }
    m_busy = false; emit stateChanged();
    if (m_pendingRefresh) QTimer::singleShot(0,this,&KeyboardSettings::refresh);
}
void KeyboardSettings::onWritten(quint64 token, KeyboardOutcome outcome, const KeyboardSnapshot &snapshot, const QString &error)
{
    if (!m_writing || token != m_token) return;
    m_writing = m_busy = false;
    const bool verified = outcome == KeyboardOutcome::Success && snapshot.available && snapshot.owner == m_writeOwner && snapshot.version == m_writeVersion && KeyboardTypes::sameValues(snapshot.values,m_requested);
    if (snapshot.available) { m_applied = snapshot; m_ready = true; } else m_ready = false;
    if (verified) {
        m_staged = m_applied; m_dirty = m_conflict = false; m_error.clear(); m_keepError = false;
        m_result = QStringLiteral("Keyboard preferences were saved and read back. %1").arg(snapshot.runtimeStatus);
    } else {
        m_dirty = !KeyboardTypes::sameValues(m_applied.values,m_staged.values); m_conflict = true; m_keepError = true;
        if (outcome == KeyboardOutcome::Uncertain || outcome == KeyboardOutcome::Success) m_lockedOwner = m_writeOwner;
        m_error = error.isEmpty() ? QStringLiteral("Keyboard changes were not fully verified. Some preferences may have been saved. Revert reloads current values.") : error;
        m_result.clear();
    }
    emit applyFinished(verified); emit stateChanged();
    if (m_pendingRefresh) QTimer::singleShot(0,this,&KeyboardSettings::refresh);
}
