#include "DisplayPage.h"
#include "Win7Ui.h"
#include "Win7FormStyle.h"
#include "settings/DisplaySettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QSize logicalSize(const DisplayOutput &output)
{
    for (const auto &mode : output.modes) if (mode.id == output.currentMode) {
        if (!std::isfinite(output.scale) || output.scale <= 0) return {};
        const double rawWidth = mode.width / output.scale, rawHeight = mode.height / output.scale;
        if (!std::isfinite(rawWidth) || !std::isfinite(rawHeight) || rawWidth > std::numeric_limits<int>::max() || rawHeight > std::numeric_limits<int>::max()) return {};
        const int width = qMax(1, qRound(rawWidth));
        const int height = qMax(1, qRound(rawHeight));
        return output.rotation == 2 || output.rotation == 8 || output.rotation == 32 || output.rotation == 128 ? QSize(height, width) : QSize(width, height);
    }
    return {};
}
QString rotationText(int rotation)
{
    switch (rotation) {
    case 1: return QObject::tr("Landscape (normal)");
    case 2: return QObject::tr("Portrait (left)");
    case 4: return QObject::tr("Landscape (inverted)");
    case 8: return QObject::tr("Portrait (right)");
    default: return QObject::tr("Unsupported orientation (%1)").arg(rotation);
    }
}
QString outputDescription(const DisplayOutput &output, int number)
{
    const auto size = logicalSize(output);
    return QObject::tr("%1. %2: %3, %4 × %5 logical pixels at (%6, %7), %8%, %9")
        .arg(number).arg(output.name, output.enabled ? QObject::tr("enabled") : QObject::tr("disabled"))
        .arg(size.width()).arg(size.height()).arg(output.position.x()).arg(output.position.y())
        .arg(QString::number(output.scale * 100, 'g', 12), rotationText(output.rotation));
}
QStringList changesText(const DisplayOutput &before, const DisplayOutput &after)
{
    QStringList changes;
    if (before.enabled != after.enabled) changes << (after.enabled ? QObject::tr("enable display") : QObject::tr("disable display"));
    if (before.currentMode != after.currentMode) {
        QString oldMode = before.currentMode, newMode = after.currentMode;
        for (const auto &mode : before.modes) if (mode.id == before.currentMode) oldMode = mode.label();
        for (const auto &mode : after.modes) if (mode.id == after.currentMode) newMode = mode.label();
        changes << QObject::tr("resolution %1 → %2").arg(oldMode, newMode);
    }
    if (before.scale != after.scale) changes << QObject::tr("scale %1% → %2%")
        .arg(QString::number(before.scale * 100, 'g', 12), QString::number(after.scale * 100, 'g', 12));
    if (before.rotation != after.rotation) changes << QObject::tr("orientation %1 → %2").arg(rotationText(before.rotation), rotationText(after.rotation));
    if (before.position != after.position) changes << QObject::tr("position (%1, %2) → (%3, %4)")
        .arg(before.position.x()).arg(before.position.y()).arg(after.position.x()).arg(after.position.y());
    return changes;
}
void renderArrangement(QLabel *preview, QLabel *description, const DisplaySnapshot &snapshot, int selectedId)
{
    QPixmap pixmap(740, 210); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    QRectF bounds; bool hasBounds = false; QStringList descriptions; int number = 0;
    for (const auto &output : snapshot.outputs) if (output.connected) {
        ++number; descriptions << outputDescription(output, number);
        auto size = logicalSize(output); if (size.isEmpty()) size = QSize(320, 180);
        const QRectF geometry(output.position, size);
        bounds = hasBounds ? bounds.united(geometry) : geometry; hasBounds = true;
    }
    if (!hasBounds) {
        painter.setPen(QColor("#555555")); painter.drawText(pixmap.rect(), Qt::AlignCenter, QObject::tr("No connected displays were reported."));
    } else {
        const QRectF area(28, 16, pixmap.width() - 56, pixmap.height() - 32);
        const double factor = qMin(area.width() / qMax(1.0, bounds.width()), area.height() / qMax(1.0, bounds.height()));
        const QPointF offset = area.center() - QPointF(bounds.center().x() * factor, bounds.center().y() * factor);
        number = 0;
        for (const auto &output : snapshot.outputs) if (output.connected) {
            ++number; auto size = logicalSize(output); if (size.isEmpty()) size = QSize(320, 180);
            QRectF rectangle(offset.x() + output.position.x() * factor, offset.y() + output.position.y() * factor,
                             qMax(36.0, size.width() * factor), qMax(32.0, size.height() * factor));
            painter.setPen(QPen(output.id == selectedId ? QColor("#E5A000") : QColor("#536F8B"), output.id == selectedId ? 3 : 1,
                                output.enabled ? Qt::SolidLine : Qt::DashLine));
            painter.setBrush(output.enabled ? QColor("#6EA4CF") : QColor("#E5E8EC")); painter.drawRoundedRect(rectangle, 3, 3);
            painter.setPen(output.enabled ? Qt::white : QColor("#555555"));
            QFont font = painter.font(); font.setPointSize(24); font.setBold(true); painter.setFont(font);
            painter.drawText(rectangle, Qt::AlignCenter, QString::number(number));
            if (!output.enabled && rectangle.height() >= 65) {
                font.setPointSize(9); font.setBold(false); painter.setFont(font);
                painter.drawText(rectangle.adjusted(3, 27, -3, -3), Qt::AlignBottom | Qt::AlignHCenter, QObject::tr("Disabled"));
            }
        }
    }
    painter.end(); preview->setPixmap(pixmap);
    const auto text = descriptions.join(QLatin1Char('\n'));
    preview->setAccessibleDescription(text); description->setText(text);
}
}

DisplayPage::DisplayPage(QScrollArea *sidebar, QWidget *parent)
    : DisplayPage(sidebar, DisplaySettings::instance(), parent) {}

DisplayPage::DisplayPage(QScrollArea *sidebar, DisplaySettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 820); Win7::styleSettingsControls(this); Win7::styleSettingsFields(this);
    layout->addWidget(Win7::pageTitle(tr("Change the appearance of your displays"))); layout->addSpacing(12);
    auto *preview = new QFrame; preview->setObjectName(QStringLiteral("displayPreview"));
    preview->setStyleSheet(QStringLiteral("QFrame#displayPreview { background: #F3F6FA; border: 1px solid #A8B4C2; }"));
    auto *previewLayout = new QVBoxLayout(preview); previewLayout->setContentsMargins(4, 4, 4, 4);
    m_monitor = new QLabel; m_monitor->setObjectName(QStringLiteral("displayMonitor")); m_monitor->setAlignment(Qt::AlignCenter);
    m_monitor->setAccessibleName(tr("Staged display arrangement, scaled to fit")); previewLayout->addWidget(m_monitor);
    layout->addWidget(preview);
    auto *previewCaption = Win7::bodyLabel(tr("Preview of the staged arrangement, scaled to fit. Choose a display below to edit it; use the position controls to arrange displays."));
    previewCaption->setTextFormat(Qt::PlainText); layout->addWidget(previewCaption);
    m_arrangementText = Win7::bodyLabel({}); m_arrangementText->setObjectName(QStringLiteral("displayArrangementDescription"));
    m_arrangementText->setTextFormat(Qt::PlainText); m_arrangementText->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(m_arrangementText); layout->addSpacing(12);
    auto *form = new QGridLayout; form->setColumnMinimumWidth(0, 155); form->setColumnStretch(1, 1); form->setVerticalSpacing(9);
    auto addField = [&](const QString &label, QWidget *control, int row) {
        auto *name = Win7::label(label); name->setBuddy(control); form->addWidget(name, row, 0); form->addWidget(control, row, 1);
    };
    m_outputs = new QComboBox; m_outputs->setObjectName(QStringLiteral("displayOutput")); m_outputs->setAccessibleName(tr("Display to edit"));
    addField(tr("&Display:"), m_outputs, 0);
    m_enabled = new QCheckBox(tr("&Enable this display")); m_enabled->setObjectName(QStringLiteral("displayEnabled")); form->addWidget(m_enabled, 1, 1);
    m_modes = new QComboBox; m_modes->setObjectName(QStringLiteral("displayResolution")); m_modes->setAccessibleName(tr("Resolution and refresh rate"));
    addField(tr("&Resolution:"), m_modes, 2);
    m_scale = new QComboBox; m_scale->setObjectName(QStringLiteral("displayScale")); m_scale->setAccessibleName(tr("Display scale as a percentage"));
    addField(tr("&Scale:"), m_scale, 3);
    m_rotation = new QComboBox; m_rotation->setObjectName(QStringLiteral("displayRotation")); m_rotation->setAccessibleName(tr("Display orientation"));
    addField(tr("&Orientation:"), m_rotation, 4);
    m_x = new QSpinBox; m_y = new QSpinBox;
    m_x->setObjectName(QStringLiteral("displayPositionX")); m_y->setObjectName(QStringLiteral("displayPositionY"));
    for (auto *position : {m_x, m_y}) { position->setRange(-DisplayProtocol::MaximumPosition, DisplayProtocol::MaximumPosition); position->setSuffix(tr(" px")); position->setAccelerated(true); }
    m_x->setAccessibleName(tr("Horizontal display position in logical pixels")); m_y->setAccessibleName(tr("Vertical display position in logical pixels"));
    addField(tr("Horizontal position (&X):"), m_x, 5); addField(tr("Vertical position (&Y):"), m_y, 6);
    m_relativeOutput = new QComboBox; m_relativeOutput->setObjectName(QStringLiteral("displayRelativeOutput")); m_relativeOutput->setAccessibleName(tr("Reference display for relative arrangement"));
    addField(tr("Arrange relative &to:"), m_relativeOutput, 7);
    auto *placementRow = new QHBoxLayout; m_placement = new QComboBox; m_placement->setObjectName(QStringLiteral("displayPlacement"));
    m_placement->setAccessibleName(tr("Place selected display relative to reference display"));
    m_placement->addItem(tr("To the right"), QStringLiteral("right")); m_placement->addItem(tr("To the left"), QStringLiteral("left"));
    m_placement->addItem(tr("Above"), QStringLiteral("above")); m_placement->addItem(tr("Below"), QStringLiteral("below"));
    m_arrange = new QPushButton(tr("&Move display")); m_arrange->setObjectName(QStringLiteral("displayArrange"));
    placementRow->addWidget(m_placement, 1); placementRow->addWidget(m_arrange);
    auto *placementLabel = Win7::label(tr("&Placement:")); placementLabel->setBuddy(m_placement); form->addWidget(placementLabel, 8, 0); form->addLayout(placementRow, 8, 1);
    layout->addLayout(form); layout->addSpacing(10);
    m_selectedSupport = Win7::bodyLabel({}); m_selectedSupport->setObjectName(QStringLiteral("displaySelectedSupport")); m_selectedSupport->setTextFormat(Qt::PlainText); layout->addWidget(m_selectedSupport);
    m_support = Win7::bodyLabel({}); m_support->setObjectName(QStringLiteral("displaySupport")); m_support->setTextFormat(Qt::PlainText); layout->addWidget(m_support);
    m_changes = Win7::bodyLabel({}); m_changes->setObjectName(QStringLiteral("displayDraftChanges")); m_changes->setTextFormat(Qt::PlainText); layout->addWidget(m_changes);
    auto *intro = Win7::bodyLabel(session->editRestrictionReason().isEmpty()
        ? tr("Changes are staged for all displays until you click Apply. You have 15 seconds to keep the preview; otherwise the previous display configuration is restored. A disabled display remains in this draft so you can enable it again.")
        : tr("Display information is available for review. Editing is withheld in this candidate; the reason appears below."));
    intro->setTextFormat(Qt::PlainText); layout->addWidget(intro);
    m_status = Win7::bodyLabel({}); m_status->setObjectName(QStringLiteral("displayStatus")); m_status->setTextFormat(Qt::PlainText); layout->addWidget(m_status);
    m_confirmation = Win7::bodyLabel({}); m_confirmation->setObjectName(QStringLiteral("displayConfirmation")); m_confirmation->setTextFormat(Qt::PlainText); layout->addWidget(m_confirmation);
    auto *confirmButtons = new QHBoxLayout; confirmButtons->addStretch();
    m_keep = new QPushButton(tr("&Keep changes")); m_keep->setObjectName(QStringLiteral("displayKeep"));
    m_undoPreview = new QPushButton(tr("Restore &now")); m_undoPreview->setObjectName(QStringLiteral("displayRevertPreview"));
    confirmButtons->addWidget(m_keep); confirmButtons->addWidget(m_undoPreview); layout->addLayout(confirmButtons);
    layout->addStretch(); auto *buttons = new QHBoxLayout;
    m_refresh = new QPushButton(tr("Re&fresh")); m_refresh->setObjectName(QStringLiteral("displayRefresh")); buttons->addWidget(m_refresh); buttons->addStretch();
    m_revert = new QPushButton(tr("Re&vert")); m_revert->setObjectName(QStringLiteral("displayRevert"));
    m_apply = new QPushButton(tr("&Apply")); m_apply->setObjectName(QStringLiteral("displayApply")); buttons->addWidget(m_revert); buttons->addWidget(m_apply); layout->addLayout(buttons);
    connect(m_outputs, &QComboBox::currentIndexChanged, this, [this](int index) { if (!m_updating && index >= 0) m_session->selectOutput(m_outputs->itemData(index).toInt()); });
    connect(m_modes, &QComboBox::currentIndexChanged, this, [this](int index) { if (!m_updating && index >= 0) m_session->selectMode(m_modes->itemData(index).toString()); });
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool value) { if (!m_updating) m_session->selectEnabled(value); });
    connect(m_scale, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating || index < 0) return;
        if (m_scale->itemData(index, Qt::UserRole + 1).toString() == QLatin1String("custom")) {
            // Finish the combo's own signals before a modal can destroy its page.
            const QPointer<DisplaySettings> session(m_session); const int output = m_session->selectedOutput();
            const auto applied = m_session->snapshot(), draft = m_session->staged();
            QTimer::singleShot(0,this,[this,session,output,applied,draft] {
                if (!session) return;
                if (session->selectedOutput() != output || session->isBusy() || session->hasConflict()
                    || !DisplayProtocol::same(applied,session->snapshot()) || !DisplayProtocol::same(draft,session->staged())) { updateState(); return; }
                editCustomScale();
            });
        }
        else m_session->selectScale(m_scale->itemData(index).toDouble());
    });
    connect(m_rotation, &QComboBox::currentIndexChanged, this, [this](int index) { if (!m_updating && index >= 0) m_session->selectRotation(m_rotation->itemData(index).toInt()); });
    for (auto *position : {m_x, m_y}) {
        connect(position,&QSpinBox::valueChanged,this,[this,position] { if (!m_updating) position->setProperty("displayPositionEdited",true); });
        connect(position, &QSpinBox::editingFinished, this, [this] {
        if (m_updating || (!m_x->property("displayPositionEdited").toBool() && !m_y->property("displayPositionEdited").toBool())) return;
        m_x->setProperty("displayPositionEdited",false); m_y->setProperty("displayPositionEdited",false);
        if (qAbs(qint64(m_x->value())) > DisplayProtocol::MaximumPosition || qAbs(qint64(m_y->value())) > DisplayProtocol::MaximumPosition) {
            updateState(); m_status->setText(tr("New positions must be between %1 and %2 logical pixels. The current native position has been retained.")
                .arg(-DisplayProtocol::MaximumPosition).arg(DisplayProtocol::MaximumPosition));
            m_status->setStyleSheet(QStringLiteral("color: #8A1C1C;")); m_status->show(); return;
        }
        m_session->selectPosition(m_x->value(), m_y->value());
    });
    }
    connect(m_relativeOutput, &QComboBox::currentIndexChanged, this, &DisplayPage::updateArrangementControls);
    connect(m_arrange, &QPushButton::clicked, this, &DisplayPage::arrangeSelected);
    connect(m_apply, &QPushButton::clicked, session, &DisplaySettings::apply); connect(m_revert, &QPushButton::clicked, session, &DisplaySettings::revert);
    connect(m_refresh, &QPushButton::clicked, session, &DisplaySettings::refresh); connect(m_keep, &QPushButton::clicked, session, &DisplaySettings::keep);
    connect(m_undoPreview, &QPushButton::clicked, session, &DisplaySettings::revert); connect(session, &SettingsSession::stateChanged, this, &DisplayPage::updateState);
    updateState();
}

SettingsSession *DisplayPage::settingsSession() const { return m_session; }
QList<SidebarLink> DisplayPage::sidebarLinks() { return {}; }
QList<SidebarLink> DisplayPage::sidebarSeeAlso() { return { Nav::to(tr("Night Light"), PageId::NightLight) }; }

void DisplayPage::updateArrangementControls()
{
    const auto &draft = m_session->staged(); const auto *output = draft.output(m_session->selectedOutput());
    const auto *reference = draft.output(m_relativeOutput->currentData().toInt());
    const bool enabled = m_session->isReady() && !m_session->isBusy() && !m_session->hasConflict() && m_session->selectedSupportText().isEmpty() && DisplayProtocol::canPosition(draft)
        && output && output->enabled && reference && reference->enabled && !logicalSize(*output).isEmpty() && !logicalSize(*reference).isEmpty();
    m_relativeOutput->setEnabled(enabled || (m_relativeOutput->count() > 0 && output && output->enabled && !m_session->isBusy() && !m_session->hasConflict() && DisplayProtocol::canPosition(draft) && m_session->selectedSupportText().isEmpty()));
    m_placement->setEnabled(enabled); m_arrange->setEnabled(enabled);
    m_arrange->setToolTip(enabled ? tr("Move the selected display beside the reference display in logical pixels. This stages the position only.")
                                 : tr("Select two enabled, editable displays to use relative arrangement."));
}
void DisplayPage::arrangeSelected()
{
    if (!m_arrange->isEnabled()) return;
    const auto &draft = m_session->staged(); const auto *output = draft.output(m_session->selectedOutput());
    const auto *reference = draft.output(m_relativeOutput->currentData().toInt()); if (!output || !reference) return;
    const auto ownSize = logicalSize(*output), referenceSize = logicalSize(*reference);
    qint64 x = reference->position.x(), y = reference->position.y(); const auto placement = m_placement->currentData().toString();
    if (placement == QLatin1String("right")) x += referenceSize.width();
    else if (placement == QLatin1String("left")) x -= ownSize.width();
    else if (placement == QLatin1String("above")) y -= ownSize.height();
    else if (placement == QLatin1String("below")) y += referenceSize.height();
    if (x < -DisplayProtocol::MaximumPosition || x > DisplayProtocol::MaximumPosition || y < -DisplayProtocol::MaximumPosition || y > DisplayProtocol::MaximumPosition) {
        updateState(); m_status->setText(tr("This arrangement would place a display outside %1 to %2 logical pixels. The draft position has been retained.")
            .arg(-DisplayProtocol::MaximumPosition).arg(DisplayProtocol::MaximumPosition));
        m_status->setStyleSheet(QStringLiteral("color: #8A1C1C;")); m_status->show(); return;
    }
    m_session->selectPosition(int(x), int(y));
}
void DisplayPage::editCustomScale()
{
    const auto *output = m_session->staged().output(m_session->selectedOutput()); if (!output || m_session->isBusy()) return;
    const QPointer<DisplaySettings> sessionGuard(m_session);
    const int id = output->id; const auto applied = m_session->snapshot(), draft = m_session->staged();
    auto *dialog = new QDialog(this); const QPointer<QDialog> dialogGuard(dialog); dialog->setObjectName(QStringLiteral("displayCustomScaleDialog")); dialog->setWindowTitle(tr("Custom Display Scale"));
    Win7::styleSettingsDialog(dialog); Win7::styleSettingsFields(dialog);
    auto *layout = new QVBoxLayout(dialog);
    auto *current = Win7::bodyLabel(tr("%1 — current scale: %2%. This changes the draft; Apply previews the display configuration.")
        .arg(output->name, QString::number(output->scale * 100, 'g', 17))); current->setTextFormat(Qt::PlainText); layout->addWidget(current);
    auto *row = new QHBoxLayout; auto *label = Win7::label(tr("Scale &percentage:")); auto *percent = new QDoubleSpinBox;
    percent->setObjectName(QStringLiteral("displayCustomScalePercent")); percent->setAccessibleName(tr("Custom display scale percentage"));
    percent->setDecimals(4); percent->setRange(DisplayProtocol::CustomMinimumScale * 100, DisplayProtocol::CustomMaximumScale * 100);
    percent->setSuffix(tr("%")); percent->setSingleStep(0.01); percent->setValue(qBound(percent->minimum(), output->scale * 100, percent->maximum()));
    label->setBuddy(percent); row->addWidget(label); row->addWidget(percent); layout->addLayout(row);
    auto *native = Win7::bodyLabel({}); native->setObjectName(QStringLiteral("displayCustomScaleNative")); native->setTextFormat(Qt::PlainText); layout->addWidget(native);
    auto *status = Win7::bodyLabel({}); status->setObjectName(QStringLiteral("displayCustomScaleStatus")); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
    auto updateNative = [&, id] {
        auto candidate = draft; if (auto *item = candidate.output(id)) item->scale = percent->value() / 100;
        const auto normalized = DisplayProtocol::normalized(candidate); const auto *value = normalized.output(id);
        native->setText(value ? tr("Native scale: %1% (rounded to 1/120). Custom values range from %2% to %3%.")
            .arg(QString::number(value->scale * 100, 'g', 12)).arg(percent->minimum()).arg(percent->maximum()) : tr("This scale could not be represented by the native display configuration."));
    };
    connect(percent, &QDoubleSpinBox::valueChanged, dialog, updateNative); updateNative();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); auto *stage = buttons->addButton(tr("&Stage scale"), QDialogButtonBox::AcceptRole);
    stage->setObjectName(QStringLiteral("displayCustomScaleStage")); buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("displayCustomScaleCancel")); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(m_session,&QObject::destroyed,dialog,&QDialog::reject);
    connect(stage, &QPushButton::clicked, dialog, [&, id] {
        if (m_session->isBusy() || m_session->hasConflict() || m_session->selectedOutput() != id
            || !DisplayProtocol::same(applied, m_session->snapshot()) || !DisplayProtocol::same(draft, m_session->staged())) {
            status->setText(tr("Display settings changed while this editor was open. Close it and review the current display before entering a custom scale.")); stage->setEnabled(false); return;
        }
        m_session->selectScale(percent->value() / 100); if (dialogGuard) dialog->accept();
    });
    const QPointer<DisplayPage> guard(this); dialog->exec(); if (dialogGuard) delete dialog; if (guard && sessionGuard) updateState();
}
void DisplayPage::updateState()
{
    m_updating = true;
    const QSignalBlocker outputBlocker(m_outputs), modeBlocker(m_modes), scaleBlocker(m_scale), rotationBlocker(m_rotation), enabledBlocker(m_enabled), xBlocker(m_x), yBlocker(m_y), relativeBlocker(m_relativeOutput);
    const auto &snapshot = m_session->snapshot(); const auto &draft = m_session->staged();
    m_outputs->clear(); int number = 0;
    for (const auto &output : draft.outputs) if (output.connected) {
        ++number; m_outputs->addItem(tr("%1. %2%3").arg(number).arg(output.name, output.enabled ? QString() : tr(" (disabled)")), output.id);
    }
    m_outputs->setCurrentIndex(m_outputs->findData(m_session->selectedOutput()));
    const auto *output = draft.output(m_session->selectedOutput()); const auto *applied = snapshot.output(m_session->selectedOutput());
    m_modes->clear(); m_scale->clear(); m_rotation->clear(); m_enabled->setChecked(output && output->enabled);
    if (output) {
        auto modes = output->modes; std::sort(modes.begin(), modes.end(), [](const auto &a, const auto &b) {
            if (a.width != b.width) return a.width > b.width; if (a.height != b.height) return a.height > b.height; return a.refresh > b.refresh;
        });
        for (const auto &mode : modes) m_modes->addItem(mode.label() + (applied && mode.id == applied->currentMode ? tr(" (current)") : QString()), mode.id);
        if (m_modes->findData(output->currentMode) < 0) m_modes->addItem(tr("%1 (unavailable current mode)").arg(output->currentMode), output->currentMode);
        m_modes->setCurrentIndex(m_modes->findData(output->currentMode));
        for (double scale : {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 3.0, 4.0}) m_scale->addItem(tr("%1%").arg(QString::number(scale * 100, 'g', 12)), scale);
        if (m_scale->findData(output->scale) < 0) m_scale->addItem(tr("%1% (%2)").arg(QString::number(output->scale * 100, 'g', 17),
            applied && applied->scale != output->scale ? tr("staged custom") : tr("current")), output->scale);
        m_scale->setCurrentIndex(m_scale->findData(output->scale));
        m_scale->addItem(tr("Custom…")); m_scale->setItemData(m_scale->count() - 1, QStringLiteral("custom"), Qt::UserRole + 1);
        for (int rotation : {1, 2, 4, 8}) m_rotation->addItem(rotationText(rotation), rotation);
        if (m_rotation->findData(output->rotation) < 0) m_rotation->addItem(rotationText(output->rotation) + tr(" (current, read-only)"), output->rotation);
        m_rotation->setCurrentIndex(m_rotation->findData(output->rotation));
        m_x->setRange(qMin(-DisplayProtocol::MaximumPosition, output->position.x()), qMax(DisplayProtocol::MaximumPosition, output->position.x()));
        m_y->setRange(qMin(-DisplayProtocol::MaximumPosition, output->position.y()), qMax(DisplayProtocol::MaximumPosition, output->position.y()));
        m_x->setValue(output->position.x()); m_y->setValue(output->position.y());
        m_x->setProperty("displayPositionEdited",false); m_y->setProperty("displayPositionEdited",false);
    } else { m_x->setValue(0); m_y->setValue(0); }
    const int referenceId = m_relativeOutput->currentData().toInt(); m_relativeOutput->clear(); number = 0;
    for (const auto &reference : draft.outputs) if (reference.connected) {
        ++number; if (reference.id != m_session->selectedOutput() && reference.enabled) m_relativeOutput->addItem(tr("%1. %2").arg(number).arg(reference.name), reference.id);
    }
    m_relativeOutput->setCurrentIndex(qMax(0, m_relativeOutput->findData(referenceId)));
    renderArrangement(m_monitor, m_arrangementText, draft, m_session->selectedOutput());
    QStringList changes;
    for (const auto &candidate : draft.outputs) if (const auto *before = snapshot.output(candidate.id)) {
        const auto edited = changesText(*before, candidate); if (!edited.isEmpty()) changes << tr("%1: %2").arg(candidate.name, edited.join(tr("; ")));
    }
    m_changes->setText(changes.isEmpty() ? QString() : tr("Pending changes:\n%1").arg(changes.join(QLatin1Char('\n')))); m_changes->setVisible(!changes.isEmpty());
    const bool editable = m_session->isReady() && !m_session->isBusy() && !m_session->hasConflict(); const QString selectedSupport = m_session->selectedSupportText();
    m_outputs->setEnabled(m_session->isReady() && !m_session->isBusy()); m_enabled->setEnabled(editable && output && selectedSupport.isEmpty());
    const bool selectedEditable = editable && output && output->enabled && selectedSupport.isEmpty();
    m_modes->setEnabled(selectedEditable && !output->followPreferredMode); m_scale->setEnabled(selectedEditable && DisplayProtocol::canScale(draft));
    m_modes->setToolTip(output && output->followPreferredMode ? tr("This display follows its preferred mode automatically; its mode is read-only.") : tr("Choose a resolution and refresh rate advertised by this display."));
    const bool supportedRotation = output && (output->rotation == 1 || output->rotation == 2 || output->rotation == 4 || output->rotation == 8);
    m_rotation->setEnabled(selectedEditable && supportedRotation && DisplayProtocol::canRotate(draft));
    m_x->setEnabled(selectedEditable && DisplayProtocol::canPosition(draft)); m_y->setEnabled(m_x->isEnabled());
    m_scale->setToolTip(DisplayProtocol::canScale(draft) ? tr("Scale text and windows on this display. The current raw scale is retained until you choose a value.") : tr("This display backend does not advertise per-display scaling."));
    m_rotation->setToolTip(!supportedRotation ? tr("The current reflected orientation is retained read-only.") : DisplayProtocol::canRotate(draft) ? tr("Stage the display orientation.") : tr("This display backend does not advertise rotation."));
    m_x->setToolTip(DisplayProtocol::canPosition(draft) ? tr("New positions accept %1 to %2 logical pixels. Current native values remain visible. Negative positions are normalized when staged.")
        .arg(-DisplayProtocol::MaximumPosition).arg(DisplayProtocol::MaximumPosition) : tr("This display backend does not advertise layout positioning.")); m_y->setToolTip(m_x->toolTip());
    m_selectedSupport->setText(selectedSupport); m_selectedSupport->setVisible(!selectedSupport.isEmpty());
    const auto support = m_session->supportText(); m_support->setText(support); m_support->setVisible(!support.isEmpty() && support != selectedSupport);
    m_apply->setEnabled(editable && m_session->isDirty() && support.isEmpty()); m_revert->setEnabled(!m_session->isBusy()); m_refresh->setEnabled(!m_session->isBusy());
    const bool confirm = m_session->awaitingConfirmation(); m_confirmation->setVisible(confirm); m_keep->setVisible(confirm); m_undoPreview->setVisible(confirm);
    m_keep->setEnabled(confirm && m_session->remainingSeconds() > 0); m_confirmation->setText(tr("Keep this display configuration? Restoring the previous configuration in %1 seconds.").arg(m_session->remainingSeconds()));
    m_status->setText(!m_session->errorText().isEmpty() ? m_session->errorText() : m_session->isBusy() && !confirm ? tr("Reading or previewing the display configuration…") : m_session->resultText());
    m_status->setStyleSheet(m_session->errorText().isEmpty() ? QStringLiteral("color: #333333;") : QStringLiteral("color: #8A1C1C;")); m_status->setVisible(!m_status->text().isEmpty());
    m_updating = false; updateArrangementControls();
}
