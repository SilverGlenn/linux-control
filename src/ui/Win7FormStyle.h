#pragma once
#include <QWidget>

void initializeWin7FieldResources();

namespace Win7 {
// Scope field colors to native Aero forms so the session's dark Qt palette
// cannot hide checkbox text, group titles or numeric input values.
inline void styleSettingsFields(QWidget *root)
{
    initializeWin7FieldResources();
    root->setStyleSheet(root->styleSheet() + QStringLiteral(
        " QCheckBox { color: #000000; background: transparent; spacing: 6px; border-image: none; }"
        " QCheckBox:disabled { color: #777777; }"
        " QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #8E8F8F; border-image: none; background: #FFFFFF; image: none; }"
        " QCheckBox::indicator:checked { image: url(:/ControlPanel/fields/check.svg); background: #E8F4FC; }"
        " QCheckBox::indicator:indeterminate { image: none; width: 9px; height: 9px; border: 3px solid #D8E3EC; background: #4F7FA1; }"
        " QCheckBox::indicator:disabled { border-color: #B8B8B8; background: #F0F0F0; }"
        " QCheckBox::indicator:indeterminate:disabled { border-color: #E0E4E8; background: #9AA6AF; }"
        " QSpinBox, QDoubleSpinBox { color: #000000; background: #FFFFFF;"
        " border: 1px solid #ABADB3; border-radius: 2px; border-image: none;"
        " padding: 3px 20px 3px 6px; selection-background-color: #CDE6F7; selection-color: #000000; }"
        " QSpinBox:focus, QDoubleSpinBox:focus { border-color: #3C7FB1; }"
        " QSpinBox:disabled, QDoubleSpinBox:disabled { color: #777777; background: #F0F0F0; border-color: #C5C5C5; }"
        " QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right;"
        " width: 18px; border-left: 1px solid #D4D4D4; background: #F1F1F1; border-image: none; }"
        " QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right;"
        " width: 18px; border-left: 1px solid #D4D4D4; background: #E8E8E8; border-image: none; }"
        " QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(:/ControlPanel/fields/spin-up.svg); width: 7px; height: 4px; }"
        " QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(:/ControlPanel/fields/spin-down.svg); width: 7px; height: 4px; }"
        " QGroupBox { color: #000000; background: transparent; border: 1px solid #B8BDC5;"
        " border-radius: 2px; border-image: none; margin-top: 12px; padding: 8px 4px 4px 4px; }"
        " QGroupBox::title { color: #000000; background: #FFFFFF; subcontrol-origin: margin;"
        " subcontrol-position: top left; left: 8px; padding: 0 4px; }"));
}
}
