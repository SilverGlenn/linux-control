#include "PersonalizationPage.h"
#include "LinkLabel.h"
#include "Win7Ui.h"
#include "settings/AeroThemeSettings.h"
#include "settings/AeroThemeContract.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOption>

namespace {
// Paint the indicator ourselves so inherited desktop image rules cannot turn
// the light Aero form into dark checkboxes. QCheckBox retains its native input
// handling, focus behavior, and accessible checked state.
class AeroCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QRect indicator(1, (height() - 13) / 2, 13, 13);
        QLinearGradient fill(indicator.topLeft(), indicator.bottomLeft());
        fill.setColorAt(0, isEnabled() ? QColor("#FAFAFA") : QColor("#EEEEEE"));
        fill.setColorAt(1, isEnabled() ? QColor("#E7EEF3") : QColor("#E5E5E5"));
        painter.setPen(isEnabled() ? QColor("#7C8994") : QColor("#B0B0B0"));
        painter.setBrush(fill);
        painter.drawRect(indicator.adjusted(0, 0, -1, -1));
        if (isChecked()) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(isEnabled() ? QColor("#245C31") : QColor("#888888"), 1.8));
            const QPoint origin = indicator.topLeft();
            painter.drawLine(origin + QPoint(2, 6), origin + QPoint(5, 9));
            painter.drawLine(origin + QPoint(5, 9), origin + QPoint(10, 3));
        }
        painter.setPen(isEnabled() ? QColor("#000000") : QColor("#777777"));
        painter.drawText(rect().adjusted(20, 0, 0, 0), Qt::AlignVCenter | Qt::TextShowMnemonic, text());
        Win7::paintFocus(this, painter);
    }
};

// The stylesheet draws light button surfaces; paint arrows after the base
// control so a host stylesheet cannot hide or replace their indicators.
class AeroSpinBox final : public QSpinBox {
protected:
    void paintEvent(QPaintEvent *event) override
    {
        QSpinBox::paintEvent(event);
        QStyleOptionSpinBox option; initStyleOption(&option);
        QPainter painter(this);
        painter.setPen(Qt::NoPen);
        const auto drawArrow = [&](QStyle::SubControl control, int direction, StepEnabledFlag step) {
            const QPoint center = style()->subControlRect(QStyle::CC_SpinBox, &option, control, this).center();
            QPolygon triangle;
            triangle << QPoint(center.x() - 3, center.y() - direction)
                     << QPoint(center.x() + 3, center.y() - direction)
                     << QPoint(center.x(), center.y() + 2 * direction);
            painter.setBrush(isEnabled() && option.stepEnabled.testFlag(step) ? QColor("#303030") : QColor("#909090"));
            painter.drawPolygon(triangle);
        };
        drawArrow(QStyle::SC_SpinBoxUp, -1, StepUpEnabled);
        drawArrow(QStyle::SC_SpinBoxDown, 1, StepDownEnabled);
    }
};

QPixmap windowPreview(const QColor &color)
{
    QPixmap image(360, 192);
    image.setDevicePixelRatio(2);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#6C849A"), 1));
    QLinearGradient glass(0, 0, 0, 96);
    glass.setColorAt(0, color.lighter(175)); glass.setColorAt(0.35, color); glass.setColorAt(1, color.lighter(145));
    painter.setBrush(glass); painter.drawRoundedRect(QRectF(0.5, 0.5, 179, 95), 4, 4);
    painter.setPen(QPen(QColor("#FFFFFF"), 1)); painter.setBrush(QColor("#FFFFFF"));
    painter.drawRect(QRectF(5, 25, 169, 65));
    painter.setPen(QColor("#101010"));
    QFont font = painter.font(); font.setPointSize(8); painter.setFont(font);
    painter.drawText(QRect(10, 3, 100, 19), Qt::AlignVCenter, QStringLiteral("Windows 7 Aero"));
    painter.setBrush(QColor("#A84032")); painter.setPen(QColor("#803A32"));
    painter.drawRoundedRect(QRectF(151, 2, 22, 16), 2, 2);
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawLine(159, 6, 165, 12); painter.drawLine(165, 6, 159, 12);
    painter.setPen(QPen(QColor("#333333"), 1));
    painter.drawLine(128, 12, 135, 12); painter.drawRect(QRect(140, 6, 6, 6));
    painter.drawText(QRect(14, 35, 150, 18), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Window color"));
    painter.setPen(Qt::NoPen); painter.setBrush(QColor("#D7E5F4")); painter.drawRect(QRect(14, 60, 126, 7));
    painter.setBrush(QColor("#E5E5E5")); painter.drawRect(QRect(14, 73, 99, 5));
    return image;
}
}

PersonalizationPage::PersonalizationPage(QScrollArea *sidebar, QWidget *parent)
    : PersonalizationPage(sidebar, AeroThemeSettings::instance(), parent) {}

PersonalizationPage::PersonalizationPage(QScrollArea *sidebar, AeroThemeSettings *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    Q_ASSERT(session);
    auto *layout = Win7::pageScaffold(this, sidebar, 20, 760);
    layout->addWidget(Win7::pageTitle(tr("Change the visuals on your computer")));
    layout->addSpacing(8);
    layout->addWidget(Win7::bodyLabel(tr("Personalize your custom Windows 7 Aero theme. Changes stay selected until you click Apply. "
        "Applying a customization also restores any changed Aero component identities.")));
    layout->addSpacing(16);
    auto *theme = new QFrame;
    theme->setObjectName(QStringLiteral("aeroThemeCard"));
    theme->setStyleSheet(QStringLiteral("#aeroThemeCard { background: #EAF3FC; border: 1px solid #A8BFDA; border-radius: 3px; }"));
    auto *themeLayout = new QHBoxLayout(theme);
    m_preview = new QLabel;
    m_preview->setObjectName(QStringLiteral("aeroColorPreview"));
    m_preview->setAlignment(Qt::AlignCenter); m_preview->setMinimumSize(180, 90);
    m_preview->setAccessibleName(tr("Illustration of the selected Aero window color"));
    themeLayout->addWidget(m_preview);
    themeLayout->addWidget(Win7::bodyLabel(tr("Windows 7 Aero\nCustom Aero components\n\nThis preview illustrates your selected glass color. "
        "Backgrounds and fonts have their own editors.")), 1);
    layout->addWidget(theme); layout->addSpacing(16);
    layout->addLayout(Win7::sectionHeading(tr("Window color"))); layout->addSpacing(10);
    auto *palette = new QGridLayout;
    const QStringList colors{ "#6b8ba9", "#54a9df", "#74c6ed", "#57bcb5", "#7da865", "#a6b95d", "#e1c45a", "#e4a449",
                              "#ce765a", "#bf707a", "#b070a0", "#9479b5", "#7b82b5", "#a9a9a9", "#5f6368", "#c9dce9" };
    const QStringList names{tr("Slate"), tr("Sky"), tr("Frost"), tr("Teal"), tr("Leaf"), tr("Lime"), tr("Sun"), tr("Amber"),
                            tr("Clay"), tr("Rose"), tr("Lilac"), tr("Violet"), tr("Twilight"), tr("Silver"), tr("Graphite"), tr("Ice")};
    for (int i = 0; i < colors.size(); ++i) {
        auto *swatch = new QPushButton;
        swatch->setObjectName(QStringLiteral("aeroColorSwatch%1").arg(i));
        swatch->setProperty("aeroColor", colors[i]);
        swatch->setAccessibleName(tr("%1 window color").arg(names[i]));
        swatch->setToolTip(names[i]); swatch->setCheckable(true);
        swatch->setFixedSize(34, 30); swatch->setFocusPolicy(Qt::StrongFocus);
        swatch->setStyleSheet(QStringLiteral("QPushButton { min-width: 0; padding: 0; border: 2px solid #7F9DB9; border-radius: 4px; background: %1; }"
            "QPushButton:checked { border: 3px solid #1B4A79; } QPushButton:focus { border: 3px dashed #000000; }").arg(colors[i]));
        connect(swatch, &QPushButton::clicked, this, [this, color = QColor(colors[i])] {
            m_session->selectValue(QStringLiteral("customColor"), color.name(QColor::HexArgb));
        });
        palette->addWidget(swatch, i / 8, i % 8); m_colors << swatch;
    }
    palette->setColumnStretch(8, 1); layout->addLayout(palette); layout->addSpacing(12);
    const auto identities = AeroThemeContract::identities();
    for (const auto &field : AeroThemeContract::fields()) {
        if (identities.contains(field.key)) continue;
        if (field.key == QLatin1String("accentColorGroup") || field.key == QLatin1String("accentColorName")) continue;
        auto *row = new QHBoxLayout;
        row->setContentsMargins(8, 3, 0, 3);
        auto *label = Win7::label(field.label + QLatin1Char(':')); label->setMinimumWidth(210); row->addWidget(label);
        QWidget *control = nullptr;
        if (field.defaultValue.metaType().id() == QMetaType::Bool) {
            auto *check = new AeroCheckBox(tr("Enabled")); control = check;
            connect(check, &QCheckBox::toggled, this, [this, key = field.key](bool enabled) { m_session->selectValue(key, enabled); });
        } else if (!field.choices.isEmpty()) {
            auto *combo = new QComboBox;
            for (const auto &choice : field.choices) combo->addItem(tr("%1 pixels").arg(choice), choice.toInt());
            control = combo;
            connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key = field.key](int i) {
                if (i >= 0) m_session->selectValue(key, combo->itemData(i));
                updateState();
            });
        } else if (field.key == QLatin1String("customColor")) {
            auto *choose = new QPushButton(tr("Choose &color…")); control = choose;
            connect(choose, &QPushButton::clicked, this, [this] {
                QColorDialog dialog(QColor(m_session->stagedValue(QStringLiteral("customColor")).toString()), this);
                dialog.setWindowTitle(tr("Window color"));
                dialog.setOption(QColorDialog::DontUseNativeDialog);
                Win7::styleSettingsDialog(&dialog);
                if (dialog.exec() == QDialog::Accepted) {
                    const QColor color = dialog.selectedColor();
                    m_session->selectValue(QStringLiteral("customColor"), color.name(QColor::HexArgb));
                }
            });
            label->setText(tr("Custom window color:"));
        } else {
            auto *spin = new AeroSpinBox; spin->setRange(field.minimum, field.maximum); control = spin;
            connect(spin, &QSpinBox::valueChanged, this, [this, key = field.key](int value) { m_session->selectValue(key, value); updateState(); });
            if (field.key == QLatin1String("aeroIntensity")) {
                auto *slider = new QSlider(Qt::Horizontal);
                slider->setObjectName(QStringLiteral("aeroIntensitySlider"));
                slider->setAccessibleName(tr("Glass color intensity"));
                slider->setRange(field.minimum, field.maximum); slider->setMinimumWidth(180);
                row->addWidget(slider); m_sliders.insert(field.key, slider);
                connect(slider, &QSlider::valueChanged, this, [this, key = field.key](int value) { m_session->selectValue(key, value); });
            }
        }
        control->setObjectName(field.key); control->setAccessibleName(field.key == QLatin1String("customColor") ? tr("Custom window color") : field.label); label->setBuddy(control);
        row->addWidget(control); row->addStretch(); layout->addLayout(row); m_controls.insert(field.key, control);
        auto *saved = Win7::bodyLabel(QString()); saved->setTextFormat(Qt::PlainText);
        saved->setObjectName(field.key + QStringLiteral("Saved")); saved->setContentsMargins(12, 0, 0, 3);
        layout->addWidget(saved); m_saved.insert(field.key, saved);
    }
    layout->addSpacing(12);
    auto *components = new LinkLabel(tr("View Aero component status"));
    connect(components, &LinkLabel::clicked, this, [this] { emit navigationRequested(PageId::ThemeComponents); });
    layout->addWidget(components); layout->addSpacing(12);
    layout->addLayout(Win7::sectionHeading(tr("Restore custom Aero defaults"))); layout->addSpacing(8);
    layout->addWidget(Win7::bodyLabel(tr("Restore defaults stages the approved Aero components, colors, transparency, blur, pointer size, "
        "and supported window-border options. Wallpaper, fonts, panel layout, and shortcuts are preserved. Click Apply to save.")));
    m_restore = new QPushButton(tr("Restore Aero &defaults")); m_restore->setObjectName(QStringLiteral("restoreAeroDefaults"));
    layout->addWidget(m_restore, 0, Qt::AlignLeft);
    connect(m_restore, &QPushButton::clicked, session, &AeroThemeSettings::stageRestoreDefaults);
    layout->addSpacing(12);
    m_status = Win7::bodyLabel(QString()); m_status->setObjectName(QStringLiteral("aeroThemeStatus"));
    m_review = Win7::bodyLabel(QString()); m_review->setObjectName(QStringLiteral("aeroThemeReview"));
    m_message = Win7::bodyLabel(QString()); m_message->setObjectName(QStringLiteral("aeroThemeMessage"));
    for (auto *label : {m_status, m_review, m_message}) { label->setTextFormat(Qt::PlainText); layout->addWidget(label); }
    auto *actions = new QHBoxLayout;
    actions->setSpacing(18);
    auto addLink = [this, actions](const QString &text, PageId page) {
        auto *link = new LinkLabel(text);
        connect(link, &LinkLabel::clicked, this, [this, page] { emit navigationRequested(page); }); actions->addWidget(link);
    };
    addLink(tr("Desktop Background"), PageId::DesktopBackground); addLink(tr("Font Settings"), PageId::FontSettings); actions->addStretch();
    m_apply = new QPushButton(tr("&Apply")); m_apply->setObjectName(QStringLiteral("applyAeroTheme"));
    m_revert = new QPushButton(tr("&Revert")); m_revert->setObjectName(QStringLiteral("revertAeroTheme"));
    actions->addWidget(m_apply); actions->addWidget(m_revert); layout->addSpacing(12); layout->addLayout(actions); layout->addStretch();
    connect(m_apply, &QPushButton::clicked, session, &AeroThemeSettings::apply);
    connect(m_revert, &QPushButton::clicked, session, &AeroThemeSettings::revert);
    connect(session, &SettingsSession::stateChanged, this, &PersonalizationPage::updateState);
    Win7::styleSettingsControls(this);
    setStyleSheet(styleSheet() + QStringLiteral(" QCheckBox { color: #000000; background: transparent; }"
        " QSpinBox { color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; border-radius: 0; padding: 3px 20px 3px 6px; min-height: 20px; }"
        " QSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 17px; height: 13px;"
        " image: none; border-image: none; border: 1px solid #ABADB3; background: #F2F2F2; }"
        " QSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 17px; height: 13px;"
        " image: none; border-image: none; border: 1px solid #ABADB3; background: #F2F2F2; }"
        " QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #DDEFFC; border-color: #3C7FB1; }"
        " QSpinBox::up-button:pressed, QSpinBox::down-button:pressed { background: #C4E5F6; }"
        " QSpinBox::up-arrow, QSpinBox::down-arrow { image: none; border-image: none; width: 7px; height: 5px; }"
        " QSpinBox:disabled { color: #777777; background: #F0F0F0; }"
        " QSlider::groove:horizontal { height: 4px; background: #D8DDE2; border: 1px solid #A0A8AF; }"
        " QSlider::handle:horizontal { width: 11px; margin: -6px 0; background: #E8F4FC; border: 1px solid #6A8BAA; border-radius: 2px; }"
        " QSlider::handle:horizontal:focus { border: 2px solid #245B96; }"));
    updateState();
}
SettingsSession *PersonalizationPage::settingsSession() const { return m_session; }
QList<SidebarLink> PersonalizationPage::sidebarLinks()
{
    return { Nav::to("Aero component status", PageId::ThemeComponents), Nav::to("Desktop Background", PageId::DesktopBackground),
             Nav::to("Font Settings", PageId::FontSettings) };
}
QList<SidebarLink> PersonalizationPage::sidebarSeeAlso()
{
    return { Nav::to("Screen Resolution", PageId::Display), Nav::to("Window Behavior", PageId::WindowBehavior),
             Nav::to("Ease of Access Center", PageId::EaseOfAccess) };
}
void PersonalizationPage::updateState()
{
    for (auto it = m_controls.cbegin(); it != m_controls.cend(); ++it) {
        const QSignalBlocker blocker(it.value()); const QVariant value = m_session->stagedValue(it.key());
        if (auto *check = qobject_cast<QCheckBox *>(it.value())) check->setChecked(value.toBool());
        else if (auto *spin = qobject_cast<QSpinBox *>(it.value())) {
            for (const auto &field : AeroThemeContract::fields()) if (field.key == it.key())
                spin->setRange(qMin(field.minimum, value.toInt()), qMax(field.maximum, value.toInt()));
            spin->setValue(value.toInt());
        } else if (auto *combo = qobject_cast<QComboBox *>(it.value())) {
            int index = combo->findData(value);
            if (index < 0) { combo->addItem(tr("Saved value: %1").arg(value.toString()), value); index = combo->count() - 1; }
            combo->setCurrentIndex(index);
        } else if (auto *button = qobject_cast<QPushButton *>(it.value())) button->setText(tr("Choose color… (%1)").arg(QColor(value.toString()).name()));
        it.value()->setEnabled(m_session->canEdit(it.key()));
        const bool color = it.key() == QLatin1String("customColor");
        const QString saved = m_session->isReady() ? (color ? QColor(m_session->appliedValue(it.key()).toString()).name() : m_session->appliedValue(it.key()).toString()) : tr("Unavailable");
        m_saved.value(it.key())->setText(tr("Saved: %1%2").arg(saved,
            m_session->appliedValue(it.key()) != value ? tr("    Selected: %1 (not applied)").arg(color ? QColor(value.toString()).name() : value.toString()) : QString()));
    }
    const QColor preview = QColor::fromHsv(qBound(0, m_session->stagedValue(QStringLiteral("aeroHue")).toInt(), 359),
        qRound(qBound(0, m_session->stagedValue(QStringLiteral("aeroSaturation")).toInt(), 100) * 2.55),
        qRound(qBound(0, m_session->stagedValue(QStringLiteral("aeroBrightness")).toInt(), 100) * 2.55));
    m_preview->setPixmap(windowPreview(preview));
    m_preview->setStyleSheet(QStringLiteral("background: transparent;"));
    for (auto *button : m_colors) {
        button->setEnabled(m_session->canEdit(QStringLiteral("aeroHue")) && m_session->canEdit(QStringLiteral("aeroSaturation")) && m_session->canEdit(QStringLiteral("aeroBrightness")));
        const QColor color(button->property("aeroColor").toString());
        button->setChecked(m_session->stagedValue(QStringLiteral("aeroHue")).toInt() == qMax(0, color.hsvHue())
            && m_session->stagedValue(QStringLiteral("aeroSaturation")).toInt() == qRound(color.hsvSaturationF() * 100)
            && m_session->stagedValue(QStringLiteral("aeroBrightness")).toInt() == qRound(color.valueF() * 100));
    }
    for (auto it = m_sliders.cbegin(); it != m_sliders.cend(); ++it) {
        const QSignalBlocker blocker(it.value());
        it.value()->setValue(m_session->stagedValue(it.key()).toInt());
        it.value()->setEnabled(m_session->canEdit(it.key()));
    }
    m_status->setText(m_session->isBusy() ? tr("Reading or applying Aero settings…") : !m_session->isReady() ? tr("Saved Aero settings are unavailable.")
        : m_session->isDirty() ? tr("Selected changes have not been applied.") : tr("Showing saved Aero settings."));
    const auto changes = m_session->changeSummary();
    m_review->setText(changes.isEmpty() ? QString() : tr("Changes to apply:\n%1").arg(changes.join(QLatin1Char('\n')))); m_review->setVisible(!changes.isEmpty());
    QStringList messages = m_session->state().issues;
    if (!m_session->errorText().isEmpty()) messages << m_session->errorText();
    if (m_session->hasConflict()) messages << tr("Settings changed outside Control Panel or Apply is uncertain. Your choices are preserved. Revert to reload before applying.");
    if (m_session->hasPartialFailure()) messages << tr("Partial or unverified Apply: some saved settings may already have changed.");
    if (!m_session->resultText().isEmpty()) messages << m_session->resultText();
    m_message->setText(messages.join(QLatin1Char('\n'))); m_message->setVisible(!messages.isEmpty());
    m_message->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(m_session->hasConflict() || !m_session->errorText().isEmpty() ? QStringLiteral("#A02C20") : QStringLiteral("#333333")));
    m_apply->setEnabled(m_session->isDirty() && m_session->canApply() && !m_session->isBusy() && !m_session->hasConflict());
    m_revert->setEnabled(!m_session->isBusy() && (m_session->isDirty() || m_session->hasConflict() || !m_session->isReady() || !m_session->errorText().isEmpty()));
    m_restore->setEnabled(m_session->isReady() && !m_session->isBusy());
}
