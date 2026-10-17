# Batch 10: native Mouse and Touchpad

The native Mouse and Touchpad Properties page uses an application-owned staged
`InputSettings` session and KWin's audited **6.6.6 Wayland** input-device D-Bus
interface. It contains Windows-style Buttons, Pointer Options, Scrolling,
Tapping and Device tabs, keyboard-accessible native controls, Apply/Revert and
per-device reported defaults. Pending choices and jobs outlive page recreation.
No alternative appearance/theme selectors are introduced.

## Supported scope

The typed whitelist covers physical pointer enablement, touchpad disable while
typing/when a mouse is connected, handedness, middle-button emulation, touchpad
click method, pointer speed and flat/adaptive acceleration, reverse scrolling,
two-finger/edge/button-held scrolling, advertised scroll buttons, scroll-speed
multiplier, tap-to-click, tap button mapping, tap-and-drag and drag lock.

Only settings with matching property types, audited writable D-Bus declarations
and positive per-device capability flags are editable. Finger features require a
touchpad and advertised tap count. Virtual devices and tablet tools/pads are not
presented as mice. Unsupported choices stay unavailable; absence is reported from
KWin's inventory rather than inferred from missing code. Existing out-of-range
values stay visible. Pointer speed has three decimal places; this matches KWin's
stored precision. The scroll-speed editor supports a bounded 0.1–10 multiplier.

KWin's mutually exclusive acceleration, click-method and scroll-method fields
are edited as native mode choices. Each requested mode generates one appropriate
setter; its coupled values are checked on readback. A scroll mode of none leaves
wheel scrolling unchanged. Device disablement is dispatched after other changes.

Restore device defaults stages the capability-supported defaults exported by
KWin. It leaves scrolling speed unchanged: the actual `ScrollFactor` default can
come from KConfig's per-class defaults and is not exported by the installed
D-Bus interface. This limitation is visible in the page. Revert discards pending
choices and reloads; it does not promise to undo already applied/external writes.

Keyboard layout/repeat, global shortcuts, tablet/touchscreen mapping, cursor
appearance, mouse button command rebinding and general workspace click/double
click preferences are separate backlog capabilities and are not completed here.

## Provider and apply behavior

The backend enumerates `devicesSysNames` from
`/org/kde/KWin/InputDevice`, reads each device using Properties.GetAll, and checks
its introspected property signatures. Only audited identity, capability, preference
and exported-default fields enter comparisons; unrelated complex D-Bus values
are excluded rather than compared as opaque QVariant objects. Manager topology and unique service owner
are bracketed around each snapshot. Every write pins the unique KWin owner,
device sysname/path/name/vendor/product/type identity, supported preference values
and capabilities. Missing or unsupported KWin versions disable writes.

A fresh full snapshot is compared before every setter. Only audited changed
properties are sent; unrelated preferences are never written. After every
acknowledgement, another snapshot must match the projected preference values,
including coupled modes. This is a read/check/Set sequence: **D-Bus provides no
atomic compare-and-swap**, and another client can race between the final read
and setter. There is no speculative rollback across clients or devices.

KWin's setters apply libinput state and request synchronous KConfig saving.
Runtime property readback confirms the values reported by KWin; this backend
does not claim independent disk-durability, physical pointer or sign-in
persistence verification. Failure or a partial save stops later writes and
reports observed state truthfully. Partial operations require explicit Revert
and reconciliation. A timed-out setter or failed readback after an acknowledged
mutation locks further writes for that KWin owner. Refresh/Revert do not cancel
a late daemon operation or remove that lock; a verified owner replacement does.

Each D-Bus call has a bounded timeout. Reads have a 12-second total budget;
Apply has a 45-second total budget and ignores late completion callbacks.
Topology/owner signals and read-only polling detect external state changes.
Modified drafts remain staged on conflicts; unmodified views refresh. Pending
writes protect navigation through the shared settings-session guard.

## Primary evidence

- Installed `/usr/share/dbus-1/interfaces/org.kde.kwin.InputDevice.xml` supplies
  the audited property names and wire types.
- [KWin 6.6.6 connection implementation](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/backends/libinput/connection.cpp)
  establishes the manager path/interface, inventory/topology signals and
  per-device configuration grouping.
- [KWin 6.6.6 device implementation](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/backends/libinput/device.cpp)
  establishes setter behavior, coupled modes, libinput acceptance and saving.
- [KWin 6.6.6 device declarations](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/backends/libinput/device.h)
  establish capability/default properties and the unexported scroll default.

## Validation

The final Debug build succeeded and all **55 CTest suites passed** (27.84
seconds). AeroQt mouse, touchpad tapping and scrolling captures were inspected.
Unsupported touchpad-only rows/tabs are hidden for a mouse, while unavailable
hardware controls remain visibly disabled. Navigation tests cover Cancel,
Discard, retained Apply operations, exit protection and confirmed results.

`input_settings_test`, `input_backend_test` and `input_page_test` use injected
sessions/transport runners and native offscreen pages. They cover staging and
view recreation, defaults, typed/capability/protocol rejection, owner/topology
changes, external edits, mode coupling, unrelated-property preservation,
partial/denied/time-out paths, total deadlines and late callbacks, independent
session readback verification, and unknown-version/read-only behavior.
`input_backend_probe` reads twice and reports snapshot stability; it performs
only discovery/read methods and never calls a
setter. Real Apply, physical behavior, persistence across sign-in, hotplug and
cross-client races remain runtime acceptance work.

The compiled read-only probe confirmed KWin 6.6.6 and four physical pointer
devices, including one touchpad. Repeated snapshots were stable. The touchpad
advertises 18 editable controls; the other three devices advertise 10, 3 and 9
respectively. No setter was called. Targeted property hashes and kcminputrc
matched exactly before/after validation.

Build, test, probe, fingerprint and screenshot evidence is retained under
build/batch10-validation. A DESTDIR staging install succeeded there; no live
installation or preference change was performed.
