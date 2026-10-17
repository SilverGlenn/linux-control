# Batch 11: native Keyboard

The Aero Keyboard page stages repeat behavior, delay/rate, startup Num Lock,
keyboard model, ordered layouts/variants/short labels, layout policy, grouped
XKB options and layout-change notifications. Apply, Revert and defaults share
an application-owned session; view destruction does not cancel a write.
Search and category links now open this native page. Keyboard accessibility
remains a separate unfinished domain.

## Audited provider

The target is installed KWin/Plasma 6.6.6 Wayland, KF6 6.24 and xkb-data 2.46.
The primary source is [Plasma Desktop's keyboard KCM](https://invent.kde.org/plasma/plasma-desktop/-/tree/v6.6.6/kcms/keyboard),
including keyboardsettings.kcfg, keyboardmiscsettings.kcfg, keyboard_config.cpp
and its QML forms, plus [KWin keyboard input](https://invent.kde.org/plasma/kwin/-/blob/v6.6.6/src/keyboard_input.cpp),
[keyboard layouts](https://invent.kde.org/plasma/kwin/-/blob/v6.6.6/src/keyboard_layout.cpp)
and [XKB configuration](https://invent.kde.org/plasma/kwin/-/blob/v6.6.6/src/xkb.cpp).
Models, layouts, variants and options come from the installed evdev XML registry
and extras; unknown saved choices are preserved until explicitly changed.

| File/group | Supported preference | KDE default / semantics |
| --- | --- | --- |
| kcminputrc / Keyboard | KeyRepeat | repeat, or accent with QT_IM_MODULE=plasmaim; nothing disables repeat |
| kcminputrc / Keyboard | RepeatDelay / RepeatRate | 600 ms / 25 Hz; ranges 100–5000 ms / 0.2–200 Hz |
| kcminputrc / Keyboard | NumLock | 2 unchanged; 0 on, 1 off; startup preference |
| kxkbrc / Layout | Model | pc104 |
| kxkbrc / Layout | Use, LayoutList, VariantList, DisplayNames | false and empty lists; labels up to three UTF-16 characters |
| kxkbrc / Layout | SwitchMode | Global; also Desktop, WinClass and Window |
| kxkbrc / Layout | Options, ResetOldOptions | empty list / false |
| kxkbrc / Layout | LayoutLoopCount | Preserved read-only: KWin Wayland ignores loop subsets |
| plasmarc / OSD | kbdLayoutChangedEnabled | true; Plasma watches configuration changes |

KWin Wayland loads LayoutList regardless of Use. Disabling custom layouts
therefore clears the list, variants and labels together. Merely saving Use=false
would mislead the user. KWin publishes ceil(RepeatRate) to Wayland clients;
the configured fractional preference is retained without promising fractional
client repeat. Num Lock is not advertised as an immediate key toggle.

KWin receives its native KConfig ConfigChanged notification using the exact
`a{saay}` format, targeted to the verified unique compositor owner. The
`org.kde.KeyboardLayouts` interface is hosted by `org.kde.keyboard` at
`/Layouts`, and getLayoutsList returns `a(sss)`. It exposes layout names, not
proof of variant/model/options/repeat/policy adoption. The layout OSD preference
uses Plasma's own plasmarc filesystem watcher, established in Plasma Workspace
6.6.6 shellcorona.cpp and osd.cpp; no invented reload method is used.

## Apply and evidence boundaries

Apply checks compositor owner/version, registry revision, config baselines and
immutable keys before writing. Each targeted write preserves unrelated keys,
checks disk readback and rechecks other values. Async calls and complete jobs
have time limits. Saved partial results retain staged choices and require
Revert; uncertain in-flight outcomes lock further writes for the same compositor
owner. A successful reload notification is reported separately from observable
layout names. No full runtime adoption is inferred from a notification.

The three keyboard suites use temporary configs, registry fixtures and private
or fake D-Bus transports. They cover native signal encoding, source-grounded
values, preservation, policy locks, conflicts, timeouts, owner replacement,
wrong readback, page recreation and staged controls. Shell integration checks
navigation prompts and jobs surviving page destruction.

The production probe is read-only by construction. It does not switch layouts,
send native notifications, set preferences or launch a KCM. Real repeat/key
behavior, layout/variant adoption, OSD display, sign-in persistence and physical
keyboard acceptance remain unverified. At the Batch 11 snapshot, global/per-layout shortcuts and a native keyboard
diagram were implementation gaps. [Batch 13](kde-batch13-shortcuts.md) now adds
registered global binding editing and a native compiled preview. Unregistered
per-layout creation/reload, additional physical geometry and live acceptance
remain open; neither batch establishes whole keyboard-module parity.

## Recorded validation — 17 October 2026

The final Debug build and staged DESTDIR install passed. All **61 CTest suites
passed** in **28.46 seconds**, including the three keyboard suites and shell
navigation integration. Aero captures of Speed, Input Languages and Advanced
were inspected; the Apply/Revert footer is present on every tab.

The read-only production probe twice read KWin **6.6.6**, the current
**English (Colemak)** layout name, and **184 models, 104 layouts and 23 option
groups** from the installed registry. Both snapshots were stable; editing was
available and the ignored Wayland loop field was read-only. This observes the
current layout metadata, not a newly applied variant or physical key effect.

Evidence lives under `build/batch11-validation` and `build/batch12-validation`:
probe JSON, interface introspection, build/test logs, Aero captures and install
staging. Hashes of **31** keyboard/default-application configuration paths were
unchanged after probes and tests. No live setters, reload notifications, layout
switches, cache rebuilds or preference writes were used for validation.
