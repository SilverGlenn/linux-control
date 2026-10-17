# Batch 18 — native power defaults and custom values

This batch extends the existing Aero display and power pages. Full installed-KDE
parity remains incomplete. No live preference changes or physical power/display
acceptance are claimed.

## Power values and native defaults

PowerDevil 6.6.6 has independent profile values for plugged in, battery and low
battery, plus global battery policy. Each supported key now retains its native
type, effective/local/inherited/user-global presence, policy and writability,
source revisions, current-value knowledge and native-editor default knowledge.
The draft records an explicit Set or Restore Default operation for each edited
key. An unchanged numeric value can still represent a meaningful request to
establish a local override; an unknown default is never reported as a verified
current setting.

The daemon's fallback and the native settings editor's fallback are separate.
PowerKCM obtains its mobile-mode input in its own process, while PowerDevil uses
its process environment and native providers. The verified adapter follows
Kirigami's exact process overrides and portal bypass/typed reply behavior. An
unreadable daemon environment leaves affected absent effective values unknown;
it does not prevent choosing a supported explicit replacement. Immutable policy,
unavailable hardware/actions, unknown schema and unwritable files still block
writes independently for each key. Changing a checkbox, action or numeric field
does not silently save its untouched companion.

Restore Defaults follows KF6 Config 6.24's native skeleton behavior:

- A changed key with an inherited/system default saves that typed default locally.
- A changed key without an inherited default uses `revertToDefault`, preserving
  native global-origin and deletion-mask semantics. A user-global-only override
  can require changing the user's `kdeglobals`; unrelated entries are preserved.
- An explicit override already equal to the editor default is a native no-op and
  remains present. An absent override is also a no-op. Cancelling an unapplied
  Set on either restores the baseline without inventing a persisted reset.
- Unknown/malformed saved numeric values or unknown defaults are not reported as
  successfully reset. A supported explicit Set can repair an unknown current
  value. A native default outside the currently offered Set choices is not
  silently replaced with a different action.

Default removal verifies the saved override's absence. It does not falsely demand
that a daemon fallback equal the editor fallback or become known. Inherited
restoration and explicit Set separately require exact typed local readback.
Untouched values, inherited defaults, immutable policy, owner and capability
identity stay guarded. The backend rereads before and after saving, then requires
PowerDevil's reload acknowledgement; a failed or uncertain operation retains the
draft until a verified Revert. KF's global cache can conservatively cause a
readback refusal when a file's millisecond timestamp is unchanged; no successful
reload is claimed in that case. Native read/write interfaces provide no atomic
compare-and-swap against another settings writer.

The defaults review scopes the selected source, all sources, battery policy or
all supported power policies. It lists changes, native no-ops and unavailable
keys before staging. Its revision includes baseline, draft and review context;
a stale review cannot overwrite a newer draft. Staging never applies settings.
Battery threshold ordering, the native 0–100% domain and unknown companions
remain checked before Apply. Custom timeouts use the audited native minima
(display 30 seconds, idle action 60 seconds, dim/locked 10 seconds) and one-week
maximum of 604,800 seconds. Never, Immediately, Same as unlocked and Default are
distinct intents; launcher sentinel values never become arbitrary custom
durations. Reset choices are enabled only when the actual restore operation can
be staged. Rejected new percentages are resynchronized rather than left looking
like an accepted draft.

## Display value domain

KScreen 6.6.5 does not offer a factory display reset: its `defaults()` reloads
current configuration and its constructor exposes Apply only. This batch retains
verified Revert rather than inventing a "recommended" layout/default algorithm.
KWin's mode/scale choice requires additional native inputs that the doctor JSON
snapshot does not expose. Preferred-mode metadata alone is not that algorithm.

The custom scale editor follows the audited KScreen 50–300% range and previews
native 1/120 quantization. Existing presets through 400% remain available. Raw
saved values outside the editor range stay visible and are preserved during
unrelated changes. Only an explicitly edited scale is quantized; exact snapshot
comparison detects even small foreign changes.

The backend's wire bounds are derived from signed Wayland 24.8 fixed-point
conversion and KWin's 1/120 readback: minimum scale 1/120, maximum writable scale
8,388,607.9916666667, maximum readable scale 8,388,608. A previously saved scale
that cannot be resent exactly may be preserved during another edit, but cannot
be replaced using a transaction that promises exact rollback. Coordinates accept
signed values through ±1,000,000 before origin normalization; normalized native
bounds and advertised screen size are checked without clamping a requested
layout. These adapter bounds are distinct from the product's scale editor range.

## Primary source contract

- [Native duration combo bounds](https://raw.githubusercontent.com/KDE/powerdevil/v6.6.6/kcm/ui/TimeDurationComboBox.qml)
- [Native duration prompt](https://raw.githubusercontent.com/KDE/powerdevil/v6.6.6/kcm/ui/DurationPromptDialog.qml)
- [PowerKCM client/default construction](https://raw.githubusercontent.com/KDE/powerdevil/v6.6.6/kcm/PowerKCM.cpp)
- [PowerDevil profile schema](https://raw.githubusercontent.com/KDE/powerdevil/v6.6.6/PowerDevilProfileSettings.kcfg)
- [PowerDevil global schema](https://raw.githubusercontent.com/KDE/powerdevil/v6.6.6/PowerDevilGlobalSettings.kcfg)
- [Kirigami mobile mode inputs](https://raw.githubusercontent.com/KDE/kirigami/v6.24.0/src/platform/tabletmodewatcher.cpp)
- [KF native skeleton write/default branch](https://raw.githubusercontent.com/KDE/kconfig/v6.24.0/src/core/kcoreconfigskeleton.h)
- [KF skeleton loading/defaults](https://raw.githubusercontent.com/KDE/kconfig/v6.24.0/src/core/kcoreconfigskeleton.cpp)
- [KF FullConfig/global synchronization](https://raw.githubusercontent.com/KDE/kconfig/v6.24.0/src/core/kconfig.cpp)
- [KF native reverted-entry origins](https://raw.githubusercontent.com/KDE/kconfig/v6.24.0/src/core/kconfigdata.cpp)
- [KF INI deletion masks](https://raw.githubusercontent.com/KDE/kconfig/v6.24.0/src/core/kconfigini.cpp)
- [KScreen Apply/defaults behavior](https://raw.githubusercontent.com/KDE/kscreen/v6.6.5/kcm/kcm.cpp)
- [KScreen custom scale field](https://raw.githubusercontent.com/KDE/kscreen/v6.6.5/kcm/ui/OutputPanel.qml)
- [libkscreen Wayland scale conversion](https://raw.githubusercontent.com/KDE/libkscreen/v6.6.4/backends/kwayland/waylandoutputdevice.cpp)
- [KWin automatic mode/scale inputs](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/outputconfigurationstore.cpp)

The installed Wayland fixed-point header is `/usr/include/wayland-util.h`
(libwayland-dev 1.24.0-2); the output-management protocol package is
plasma-wayland-protocols 1.20.0-2.

## Validation

The final Debug build passes all **73 CTest suites in 95.17 seconds**; the
preceding complete run also passed (92.98 seconds). Final no-work build and
DESTDIR staging pass. No live installation was performed. All eight final Aero
captures were inspected, including the three new custom-scale/custom-timeout/
defaults dialogs. Unknown checkboxes have a distinct indeterminate state, and
the defaults review uses wrapped descriptions, full tooltips and readable header
text despite the session's dark palette.

Private fixtures cover native cascade/global/default/no-op semantics, malformed
and unknown values, same-number explicit overrides, companion preservation,
whole battery-pair validation, native one-week timeout bounds, stale reviews,
operation-aware readback and uncertain outcomes. UI regressions cover unavailable
reset choices, rejected raw percentage edits, staged/current labels, Cancel,
source/page recreation and destruction with either custom dialog open. Custom
dialogs open after combo signals finish, preventing deletion of a still-emitting
widget. Display protocol/watchdog tests cover exact raw-scale preservation,
foreign changes, native wire bounds and non-restorable scale replacement.

Repeated read-only production discovery matches the initial Batch18 report:
one eDP-1 panel with eleven modes, all three power profiles and **54 key records**.
The native editor defaults are known for all records; eleven absent
mobile-dependent effective values remain unknown, with supported explicit
replacements available. Fifteen profile keys per source and three global keys
are writable on this installation; keyboard backlight is not advertised. This
proves bounded discovery/configuration knowledge, not physical effects or live
reload adoption.

The final live comparison finds **29 of 31 monitored paths unchanged**, with no
new application entries or service-cache files. Two KWin files changed during
validation: `kwinoutputconfig.json`'s stored brightness changed from 0.75 to 0.70,
and `kwinrc` gained `[Wayland] VirtualKeyboardEnabled=true`. Their cause remains
unattributed. The audited commands/probe paths invoke no live preference setters,
reloads, registration, power actions, inhibition, application launch or cache
rebuild; indirect runtime effects are not excluded. Both live files were
preserved. Ten private before copies and two private after copies retain evidence
with owner-only permissions. The observed drift does not count as accepted live
write verification.

Ignored `build/batch18-validation` retains build/focused/full suite logs and
detail, inspected captures, repeated probe snapshots/summary, file fingerprints,
exact drift descriptions, private before/after bytes and the staged install.
Initial failures and their corrections are retained in the earlier logs. Real
multi-monitor recovery, hardware effects, daemon adoption, sign-in persistence
and remaining installed-module controls continue to block full KDE parity.
