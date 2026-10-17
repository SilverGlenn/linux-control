# Batch 6: native Personalization and Fonts

This batch expands everyday appearance controls for the recorded KDE Wayland
installation. It does not close the complete Milestone 2 parity gate. Batch 5's
account, Aero extension and Ubuntu package feasibility work followed this batch,
as requested after implementation of Batch 6 had begun. Its scope and evidence
are in [Batch 5 backend proofs](kde-batch5-backend-proofs.md).

Release policy changed on 17 October 2026: the finished product will allow only
the project's custom Aero theme and approved components. The generic selectors
described here document development proofs, not release theme choices. Their
unrestricted choices must be removed or gated before release. Earlier backlog
items for alternative theme/decoration selection and global-theme presets are
superseded by [Aero-specific tuning and restoration](kde-settings-replacement-plan.md#aero-only-release-requirement-17-october-2026).

Batch 7 now replaces these generic selectors and retires their production write
paths. See [Aero-only Personalization](kde-batch7-aero.md) for the current surface.

## Delivered controls

| Native surface | Implemented | Remaining scope |
| --- | --- | --- |
| Personalization | Existing staged color schemes; native links to background, appearance components and font settings | Full global themes, detailed colors, sounds, lock screen |
| Desktop Background | Existing desktop selection; local image; Fill, Fit, Stretch, Center; preflight in the same Plasma script that writes; Plasma readback | Slideshows, dynamic/package wallpaper editing, inactive activity creation, rendered-image verification |
| Appearance Components | Installed KDE icon and cursor themes; Qt widget styles; individual staged values, policy checks, readback, reload notifications | Plasma desktop style, decoration selection and options, component installation, GTK style, full live-adoption verification |
| Font Settings | General, fixed, small, toolbar, menu, window-title fonts; local fontconfig antialias, hinting and subpixel overrides | X11 resource export, DPI, conditional-rule editor, full cross-application rendering verification |
| Fonts | Async inventory; search, arrows/Enter/Space; native editable preview; validated user font install; removal of unchanged app-managed files; cache refresh and rescan | System/unmanaged font management, enable/disable, duplicates, all-face/variable-font controls |

All staged editors have application-owned sessions, Apply/Revert, external-change
conflicts and the shared navigation/exit guard. Current and selected values are
shown separately. Font inventory and mutation services outlive their views and
retain results in the work strip and Details.

## Integration evidence

The target remains Plasma Workspace 6.6.6, Qt 6.10.2, KF6 Config 6.24 and
fontconfig 2.17.1. These capabilities were inspected against official sources
and the installed configuration; no live preference was changed for validation.

### Desktop backgrounds

KDE's [plasma-apply-wallpaperimage implementation](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/wallpapers/image/plasma-apply-wallpaperimage.cpp)
uses `org.kde.PlasmaShell.evaluateScript`, changes `org.kde.image`, and writes
`Image`/`FillMode`. The native backend uses the same scripting interface, scoped
to a selected existing containment ID instead of all desktops. It checks ID,
screen, plugin, image, fill mode and lock state within the write script before
changing anything. JSON encoding escapes user data and JavaScript line separators.

[ScriptEngine::V1](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/shell/scripting/scriptengine_v1.cpp)
returns global immutability as the strings `mutable`, `userImmutable`, and
`systemImmutable`; these map explicitly to the installed Plasma enum values
1/2/4. User widget locking does not prevent wallpaper changes; system policy does.
KConfig group/entry immutability and file writability are also checked. Inactive
desktops are visible but unavailable for writes.

[Applet::writeConfig/reloadConfig](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/shell/scripting/applet.cpp)
updates wallpaper configuration and requests normal Plasma configuration sync.
Readback confirms Plasma's current preference. It does not prove completed disk
sync or compare rendered pixels. There is no automatic stale-state rollback after
an uncertain D-Bus result. A timeout/failure remains an explicit conflict until
Revert rereads the actual setting. Existing unrelated wallpaper options and
other desktops are preserved.

### Appearance components

The official [6.6.6 source archive](https://download.kde.org/stable/plasma/6.6.6/plasma-workspace-6.6.6.tar.xz)
and its [icon settings](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/icons/iconssettings.cpp),
[cursor settings schema](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/cursortheme/cursorthemesettings.kcfg),
[style schema](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/style/stylesettings.kcfg),
and [notification helper](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/kcms-common.cpp)
establish these keys and reload requests:

- `kdeglobals [Icons] Theme`, public KIconTheme/KIconLoader cache notifications.
- `kcminputrc [Mouse] cursorTheme`, `notifyChange(5, 0)`.
- `kdeglobals [KDE] widgetStyle`, `notifyChange(2, 0)`.

Choices come from installed themes, libXcursor's read-only search path and
QStyleFactory. Missing current values stay visible without being silently
replaced. Style creation is validated before writing. All changed fields are
checked before the first write, and only those keys are saved. Readback confirms
persistence; notifications request adoption. KDE's internal X11 resource export
is not duplicated, and restart/sign-out requirements remain explicit.

### Fonts and user font files

[FontsSettings](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/fonts/fontssettings.kcfg)
defines the six font roles. [FontsAASettings](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/fonts/fontsaasettings.cpp)
and [KXftConfig](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/fonts/kxftconfig.cpp)
establish the user fontconfig file selection, XML rule forms and mirrored
`General/Xft*` keys. [Fonts KCM](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/fonts/fonts.cpp)
sends `/KDEPlatformTheme` `org.kde.KDEPlatformTheme.refreshFonts` after saving.
This is a notification, not an acknowledgement that every application adopted
the fonts. X11 Apply is disabled because its additional resource export is not
implemented. Existing applications may need restart or sign out/in.

Preflight revisions include KDE config cascades and fontconfig includes. Only
supported unconditional local rendering rules are edited. Conditional rules,
aliases, includes and unrelated settings are preserved; unsupported XML rules
fail closed. New general rules precede related conditional exclusions. KDE and
fontconfig files cannot be committed atomically together: a partial save or
failed refresh is read back and reported, with no fabricated rollback.

Font files are validated from bounded descriptor-read bytes, copied into
`$XDG_DATA_HOME/fonts/controlpanel`, and published without overwrites. Removal
requires an app record matching filename, inode, device and content hash. Symlink,
hardlink, unmanaged and replaced files are refused. No system helper or elevated
privilege is involved. Only this directory is passed to `fc-cache -f`; a fresh
`fc-list` inventory follows cache success or failure, and missing detection is
reported as unverified. A bounded copy/validation step is synchronous; inventory
and cache tools are asynchronous with output and time limits.

## Validation

Tests use temporary files, fake reload services/tools and offscreen native pages.
Wallpaper scripts execute against independent JavaScript fixtures, including
quoted/newline paths, actual Plasma lock strings, racing edits and wrong-target
protection. Session/backend tests cover readback failure, immutability, absent
tools/services, external edits and uncertain partial writes. Font tests cover
conditional-rule preservation and file identity/path protections. Shared shell
tests cover discovery/search, Apply/Discard/Cancel and navigation persistence.

The manual `kde_backend_probe` reads real capabilities without selecting values,
applying settings, sending reloads or refreshing font caches. Screenshot captures
use injected settings and fake inventory tools.

Final combined Batch 5/6 verification: application build succeeded and all 42
CTest suites passed. The live read-only probe reports background editing,
3 installed icon themes, 9 cursor themes and 6 application styles available,
plus the current Segoe UI font with antialiasing, slight hinting and RGB subpixel
overrides. A production-only fontconfig include-directory issue found by that
probe was fixed and covered by a regression; directories contribute membership
to conflict revisions without being parsed as XML files. Native screenshots
and the staged installation were inspected. No live appearance/font preference
or real font installation/removal was used to verify this work.
