# Batch 17: display layout and native power policies

This increment expands the existing native Aero editors. It does not establish
full KDE Display Configuration or Power Management parity. The installed KDE
Wayland session remains the first release target; real Apply, recovery, physical
button/lid behavior and sign-in persistence still require acceptance testing.

## Display transaction

The persistent display session stages a complete configuration across connected
outputs: advertised fixed mode/refresh, output enablement, logical arrangement,
fractional scale and normal/left/inverted/right orientation. Switching the
selected monitor retains every draft. Numeric placement and relative-arrangement
controls complement the monitor preview and are keyboard accessible. Bundled
checkbox/spinner artwork and locally scoped form colors preserve readable Aero
fields independently of a dark Qt palette. Staged
positions/scales/priorities use the native canonical representation. An all-off
draft cannot be applied and can be repaired by enabling another monitor.

Apply rereads the baseline before dispatching one bounded argument batch to
`kscreen-doctor`. An independent helper owns the confirmation deadline, including
the initial native request and readback. Keep succeeds only after another exact
readback; timeout, Revert, parent pipe EOF or loss of the UI initiate rollback.
Partial native changes are eligible for rollback only when every changed field
still belongs to the baseline/requested transaction. External layout changes,
topology/catalog changes or provider replacement stop stale restoration.
Unrelated brightness/color values are read afresh and preserved.

Connector identities and native service/package identity are checked. The
serializer does not expose EDID/UUID, so an indistinguishable monitor replacement
on the same connector between bounded reads remains a limitation. Native Wayland
submission offers no compare-and-swap that isolates our final read from an outside
change just before submission; fresh-read checks cannot eliminate that race. Existing mirroring is read-only. Automatic preferred-mode outputs retain their
automatic policy and cannot change mode here; reflected transforms are retained
although the orientation selector offers four normal rotations. Scale editing
currently offers 50–400 percent and placement uses signed 16-bit input coordinates;
those product bounds do not claim the entire native domain. Color,
HDR, VRR, color profiles, night light, primary/priorities editing, replication
and additional display controls remain in the capability backlog.

The previous read-only timeout was reproduced with `QT_QPA_PLATFORM=offscreen`:
KScreen could not select a native backend. Read-only native Wayland discovery
succeeded. Production doctor children select Wayland only after verifying the
actual session context; the test helper continues to use an isolated fake
executable. Historical Batch 7 evidence remains historical.

## Power policies

The existing AC, battery and low-battery drafts now include supported idle
actions, dimming, saved display and keyboard brightness, locked-screen display
timeout, lock before display-off, short/long power-button actions, lid behavior,
external-monitor lid inhibition and suspend mode. Global low/critical battery
levels and critical-battery actions share the same Apply transaction. Immediate
performance-profile selection remains a separate operation.

Choices come from the audited native action models and current provider
capabilities. Sleep, hibernate, hybrid suspend and suspend-then-hibernate are
gated separately; lid and battery controls require their own hardware facts.
Unsupported saved values remain visible and are preserved during unrelated
edits. Choosing an idle action does not replace its saved timeout. KDE has no
editable Sleep-button action in this schema; its physical Sleep key uses Sleep
and the configured suspend mode.

Locked display timeout uses KDE's semantics: `-2` means the unlocked timeout,
`0` means immediately, and positive values start at ten seconds. A negative
saved value is never labelled Never. Critical level may equal low level, as in
the native control; changed levels must satisfy `0 <= critical <= low <= 100`.

Capabilities and service owner are pinned into the snapshot and rechecked at
the write boundary. Immutable entries, unavailable features, unknown dynamic
defaults, changed capability metadata and stale files reject writes before
mutation. Only owned changed keys are saved. Success requires the pinned
PowerDevil reload signal and method reply plus an exact fresh file/capability
readback. Saved-but-unacknowledged and partial outcomes remain pending until
explicit Revert; page destruction cannot discard the job or draft. A failed
Revert read also retains the draft and stops pending navigation without repeatedly
showing the discard prompt.

Automatic reset/defaults UI, activity overrides, power inhibition management,
peripheral battery notifications, charge thresholds/health, scripts, per-profile
performance policy and further native/provider controls remain open. Reading a
profile does not prove that inhibitors allow an idle action to occur.

## Primary source contract

Display behavior is reconciled with the installed libkscreen-bin 6.6.4 doctor,
libkscreen 6.6.4 serializer/Wayland backend and KScreen 6.6.5 frontend and KWin 6.6.6 output protocol. Power
keys, defaults, actions and capability gates are reconciled with PowerDevil
6.6.6, including its profile/global schemas, defaults, action models and native
action implementations.

- [KScreen doctor source](https://raw.githubusercontent.com/KDE/libkscreen/v6.6.4/src/doctor/doctor.cpp)
- [KScreen serializer](https://raw.githubusercontent.com/KDE/libkscreen/v6.6.4/src/configserializer.cpp)
- [PowerDevil profile schema](https://invent.kde.org/plasma/powerdevil/-/blob/v6.6.6/PowerDevilProfileSettings.kcfg)
- [PowerDevil global schema](https://invent.kde.org/plasma/powerdevil/-/blob/v6.6.6/PowerDevilGlobalSettings.kcfg)
- [PowerDevil defaults](https://invent.kde.org/plasma/powerdevil/-/blob/v6.6.6/daemon/powerdevilsettingsdefaults.cpp)

## Validation

The final Debug build passes all **73 CTest suites in 30.50 seconds**. The
preceding complete run also passed (32.15 seconds). The optional fake-provider
capture test passes, and all five final 1060×840 display/layout-confirmation and
power overview/actions/battery captures were inspected. Visual inspection caught
and corrected inherited dark checkbox labels, group titles and numeric fields;
checkbox/spinner SVG assets are bundled with the app. A direct-page test target's
initial missing resource-link dependency was corrected by sharing the form asset
library. Final no-work rebuild and DESTDIR install staging both pass; no live
installation was performed.

Protocol, controller, UI and independent-helper fixtures cover complete drafts,
per-field capability/policy guards, stale snapshots, failed Revert retention,
page destruction during work, verified Keep/timeout/EOF recovery, partial native
changes and three-output common-origin normalization. Foreign changes remain
ineligible for rollback. Power tests cover typed/malformed capability replies,
owner replacement, hostile requested metadata, owned-key preservation and
reload acknowledgement/readback. Tests use private buses, fake subprocesses and
temporary files. The first focused private-bus run encountered the sandbox's
socket restriction; the isolated run and both complete runs outside that socket
restriction pass.

Repeated read-only production discovery is stable: one eDP-1 output with eleven
modes and audited KWin 6.6.6/libkscreen 6.6.4 identity; layout, scaling and rotation
capabilities are reported. All three PowerDevil profiles and global battery
threshold/action capabilities read successfully. Keyboard backlight is not
advertised. The pinned PowerDevil process environment cannot be read, so absent
mobile-dependent keys remain read-only, including short-press button and some
battery timeout defaults. A separate metadata-only check confirms the permission
failure without publishing or retaining environment values. Future native
explicit-override/default-presence modeling must resolve that usability gap.

All **31 monitored live content/symlink fingerprints remain unchanged**, with no
added application entries or service-cache files. Ten private pre-change config
copies are retained with owner-only permissions. No live preference write,
registration/cleanup, service reload, suspend, button action, inhibition,
application launch, shortcut invocation or cache rebuild was performed.

Ignored `build/batch17-validation` retains build/focused/full suite logs and
full-run detail, final captures, stable read-only snapshots/summary, the bounded
default-input diagnostic, unchanged fingerprints and staged install. Defaults
and arbitrary custom timeout/scale entry, explicit overrides of unresolved
effective defaults, further module controls, real multi-monitor recovery,
physical power effects and sign-in persistence remain open. Full installed-KDE
parity is not complete.
