# Batch 7: Aero-only Personalization

The application now offers the project's custom Aero theme throughout its
appearance settings. Personalization replaces the generic scheme picker with
native Aero controls. Aero Components shows the approved component identities
and saved drift; it offers no alternative theme choices. Existing wallpaper and
font editors remain available separately.

## Approved contract and customization

`AeroThemeContract` defines a versioned whitelist of configuration fields,
identities, types, ranges, defaults and approved installed asset fingerprints.
The target remains the recorded Plasma/KWin 6.6.6 Wayland installation. Component
names alone do not establish compatibility: missing or changed assets disable
Apply until the contract is deliberately updated or the installation repaired.
Asset inspection reads bytes and plugin metadata without loading plugins or
executing theme scripts.

The approved components are `authui7`, `Seven-Black`, `kvantum` with
`Windows7Aero`, `Aero`, `Windows 7 Aero`, `aero-drop`, and
`org.smod.smod` / `SMOD` with resource `Aero`. The approved Aero color palette is also part of the
baseline; the reflection texture is pinned to the effect’s embedded Aero resource.
Changing only `General/ColorScheme` would leave the effective palette
unrepaired.

Native customization covers the installed Aero Glass effect's color and HSV
controls, intensity, transparency, reflection intensity, corner glow, basic and
maximized-window colorization, and blur strength. Cursor size is restricted to
the installed Aero cursor sizes. SMOD shadow and close-button outline options
use their verified native configuration keys. Palette-selection metadata is
kept consistent with the native effect's configuration; it does not select a
different KDE theme.

The field definitions come from the installed Aero sources at
`aeroshell-kwin-components` commit
`97cc934b54aa9b237a8b814cb86f93f7d33b5ebf`, including the Wayland effect's
`blur.kcfg` and KCM, and the installed SMOD configuration schema. This does not
claim complete coverage of every Aero extension option.

## Apply, repair and restoration

All appearance views share one application-owned `AeroThemeSettings` session.
Editing stages changes. **Restore Aero defaults** stages the pinned component
identities, palette and supported tuning defaults. A pending-change summary
shows the proposed changes before **Apply** saves them. **Revert** discards
pending choices and rereads saved preferences.

Restoration preserves wallpaper, fonts, panel/desktop layout, activities and
unrelated KDE or extension preferences. It never runs a global-theme installer
or executes a look-and-feel defaults script. It repairs configuration for
already approved assets; it does not download or replace missing asset files.

Apply rechecks the complete saved baseline, approved assets, supported runtime,
and configuration policy before writing. Each field is checked again at its
write boundary. External changes retain pending choices and require Revert.
Configuration files cannot be committed atomically together: partial writes or
failed reloads are reported and read back, with no fabricated rollback or
success claim. Failed Apply keeps navigation on the editor.

Readback verifies saved preferences. Runtime notifications request adoption;
they do not prove every application or rendered pixel changed. Some applications
and pointers require restart or sign out/in. Plasma's desktop-style watcher is
notified through KConfig rather than resetting the desktop layout.

## Policy boundaries

The action catalog, search, history routes and accessibility links use native
Aero editors or visibly unavailable controls. The central external launcher
rejects unrestricted appearance modules and theme-apply tools. The legacy
scheme, component and blur production writers are retired and direct callers to
Personalization. Backend validation also rejects unapproved values and unknown
fields, including requests that bypass the UI.

The running application retains its explicit AeroQt style and Aero icons after
desktop theme notifications. This is an application policy. Independent KDE
tools and administrators remain able to change desktop preferences; detected
drift is presented for explicit repair.

## Validation

Validation uses temporary configuration files, fake runtimes and asset
fixtures, in-memory sessions and offscreen native pages. No live settings Apply,
runtime reload or system installation is part of the validation workflow.
The manual `kde_backend_probe` reads installed capabilities only.

The final Debug build succeeded and all **45 CTest suites passed** (13.97 seconds).
Coverage includes direct policy rejection, asset bytes and resolution precedence,
palette cleanup with unrelated-setting preservation, real timeout/late replies,
SMOD's implicit Aero resource default, staged defaults, conflicts, partial
failure, navigation and keyboard interaction. Offscreen visual captures passed;
the Personalization and restoration views were inspected with AeroQt active,
including light checkboxes, numeric controls and the native window preview.

The read-only installed-KDE probe confirmed approved assets and compatible
runtime with no capability errors. SMOD correctly reports its implicit `Aero`
resource. The saved palette differs from the explicit contract in two extra
empty disabled-effect entries and an absent `shadeSortColumn` preference; the
repair view reports configuration differences without claiming a visual fault.
All seven inspected live configuration hashes remained unchanged.

Build/test/probe logs and captures are under `build/batch7-validation` in the
development checkout. Packaging was verified using a DESTDIR staging install
there; no live installation or settings mutation was performed.
