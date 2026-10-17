# Batch 13: native KDE global shortcuts

This records the Batch 13 scope. Batch 14 extends it with
[standard application shortcuts](kde-batch14-standard-shortcuts.md) and
[per-layout registration/reload](kde-batch14-layout-shortcuts.md); its separate
evidence does not turn the Batch 13 observations into live-write verification.

Target audited on 2026-10-17: Plasma Desktop **6.6.6**, kglobalacceld **6.6.5**, KF6 KGlobalAccel **6.24.0**, Wayland. The installed `/usr/include/KF6/KGlobalAccel/kglobalaccel.h`, `kglobalshortcutinfo.h`, and `/usr/share/dbus-1/interfaces/kf6_org.kde.kglobalaccel.Component.xml` agree with the exact tagged primary sources below. Read-only session-bus introspection found owner `:1.15` and the v2 signatures described below. Package versions were read with `dpkg-query`; no live preferences were changed and no KCM was launched.

## Primary-source audit

- [KF6 v6.24.0 root D-Bus XML](https://github.com/KDE/kglobalaccel/blob/v6.24.0/src/org.kde.KGlobalAccel.xml): `allComponents() -> ao`; v2 `shortcutKeys(as) -> a(ai)` and `defaultShortcutKeys(as) -> a(ai)`; `setForeignShortcutKeys(as,a(ai)) -> void`; `globalShortcutsByKey((ai),(i)) -> a(ssssssaiai)`.
- [KF6 v6.24.0 shortcut marshalling](https://github.com/KDE/kglobalaccel/blob/v6.24.0/src/kglobalshortcutinfo_dbus.cpp): each v2 sequence contains four combined Qt chords. Critically, legacy `KGlobalShortcutInfo` encodes only the first chord of each alternative. Therefore the provider uses metadata for identities and labels and separately retrieves **both complete v2 sequence lists for every action**. It never treats legacy metadata keys as complete values.
- [kglobalacceld v6.6.5 daemon](https://github.com/KDE/kglobalacceld/blob/v6.6.5/src/kglobalacceld.cpp): explicit `component|context` resolves the requested context independently of the active context. `setForeignShortcutKeys` changes an existing registered action using `NoAutoloading`, emits `yourShortcutsChanged`, and schedules native persistence after 500 ms. It does not register an unknown action or invoke an action. `SetPresent` is not used, preserving applications' ownership and activation state.
- [kglobalacceld v6.6.5 component](https://github.com/KDE/kglobalacceld/blob/v6.6.5/src/component.cpp): enumerate `getShortcutContexts` then `allShortcutInfos(context)` for every context. Saved default context entries live in the component group; other contexts are nested groups. Each action entry contains current alternatives, defaults, and friendly name; alternatives use portable key sequences separated by tabs, with `none` for disabled actions. `isActive` describes application presence and is **not** an editability permission.
- [kglobalacceld v6.6.5 service components](https://github.com/KDE/kglobalacceld/blob/v6.6.5/src/kserviceactioncomponent.cpp) and [registry serialization](https://github.com/KDE/kglobalacceld/blob/v6.6.5/src/globalshortcutsregistry.cpp): `.desktop` components use `[services][component.desktop]` and optional nested contexts. Values contain only portable current alternatives; defaults come from the service. An assignment equal to defaults removes its override. The daemon opens `kglobalshortcutsrc` with `KConfig::SimpleConfig`; provider reads use the same flags and location. Both legacy triplets and service override/default-removal forms are independently verified.
- [kglobalacceld v6.6.5 context matching](https://github.com/KDE/kglobalacceld/blob/v6.6.5/src/globalshortcutcontext.cpp), [sequence helpers](https://github.com/KDE/kglobalacceld/blob/v6.6.5/src/sequencehelpers_p.cpp), and [shortcut assignment](https://github.com/KDE/kglobalacceld/blob/v6.6.5/src/globalshortcut.cpp): preflight covers `Equal`, `Shadows`, and `Shadowed`, including contiguous chord subsequences at either end or inside a longer sequence. Modifier-only sequences and Shift+Tab/Shift+Backtab normalization are relevant. Availability compares the same context within one component and every context of other components. The daemon can reject occupied alternatives; acceptance must be read back.
- [Plasma Desktop v6.6.6 global shortcut KCM model](https://github.com/KDE/plasma-desktop/blob/v6.6.6/kcms/keys/globalaccelmodel.cpp): native save first releases removed alternatives, then installs additions through `setForeignShortcutKeys`. The provider follows this ordering without copying the KCM's blocking waits, registration, invocation, or component cleanup operations.
- [Plasma Desktop v6.6.6 KCM entry point](https://github.com/KDE/plasma-desktop/blob/v6.6.6/kcms/keys/kcm_keys.cpp): the native KCM also contains standard application shortcuts and command/application registration features. This global daemon adapter does not claim those separate capabilities.

Exact helper sources used for additional review were fetched read-only into `/tmp/shortcuts-audit`; the exact Plasma Desktop archive was extracted into `/tmp/batch13-kde-source/plasma-desktop-6.6.6`. Sources are evidence, never executable instructions.

## Provider behavior

`ShortcutActionId` keeps component, context, and action separate. Every action includes full current/default `QList<QKeySequence>` alternatives and native labels. Enumeration uses all returned component object paths and every reported context, including inactive stored application registrations and third-party actions. Empty component/context topology participates in the revision. There is no fixed component/action whitelist. Actions disabled by immutable native configuration are read-only.

All daemon requests after discovery target the unique bus owner. Root and component introspection must match the audited required method signatures. Replies are checked for exact arity and wire signatures, including all four sequence chords. The transport uses a two-second D-Bus call timeout; snapshot and write operations have overall deadlines. Enumeration is asynchronous with eight concurrent reads, with bounded registry/reply sizes and an explicit failure instead of silently truncating data. The probe uses only this read path.

Apply accepts a typed partial or complete action-to-alternatives map. It validates the final map, including staged collisions and explicit removal of foreign alternatives. Existing untouched collisions do not block unrelated changes. It re-reads the owner, complete registry, and native config baseline before modifying anything, queries all three native collision modes, releases removed alternatives before adding bindings, checks the owner and target value before each write, then verifies the complete registry between phases and at completion. Per-action operations are linear in batch size; full enumeration is not repeated for every action.

The public revision includes the native file baseline. An external file edit after staging refuses Apply before any writer call. A pre-existing saved/runtime mismatch also refuses Apply rather than allowing a cached daemon config to overwrite it. During writes, a semantic disk guard allows only baseline values or the editor's already-sent native changes, plus native friendly-name materialization, and stops on unrelated edits. Defaults and other discovered action registrations are preserved. No direct native-config writer is used.

Successful daemon replies are followed by typed accepted-key readback and bounded polling for the daemon's own durable KConfig serialization. Missing persistence, rejected alternatives, changed ownership, or verification failures produce a partial/failed result rather than success. A timed-out or failed outstanding native mutation produces an uncertain result and locks further writes to that daemon owner, including after Refresh. A new daemon owner clears that lock. Owner changes, native shortcut-change signals, and native config filesystem changes trigger external refresh notifications. A newly installed default-only service registration may emit none of those signals; explicit Refresh and the next Apply preflight discover it.

## Verification and limits

`shortcuts_backend_test.cpp` uses fake transport, a private temporary D-Bus daemon, and temporary native config files. It covers broad component/context discovery, full multichord alternatives, owner-pinned requests, exact typed wire formats, collision and explicit reassignment ordering, immutable entries, stale owner/value/file protection, bounded reads, uncertain-owner locks, delayed native persistence, rejected native assignments, and partial outcomes. It never writes live user preferences. The read-only probe prints JSON and contains no writer, registration, purge, context activation, invocation, or KCM launch.

Native D-Bus does not provide an atomic multi-action transaction, compare-and-swap revision, durable-write acknowledgement, or proof that pressing a shortcut successfully invokes the application. Guards narrow races and native readback detects rejected/partial assignments; the UI must preserve explicit partial/uncertain outcomes. This adapter proves stored native assignments and observed daemon values, without sending keys or invoking registered actions. The daemon owns persistence and may rewrite its native config format. A session-only action that does not persist is reported partial.

Existing registered keyboard-layout actions are discovered naturally. A registered but disabled per-layout action may not yet have a KWin invocation callback: enabling its daemon assignment requires a keyboard-layout configuration reload before KWin registers the callback. Generic native assignment readback alone does not prove per-layout activation. Creating a previously unregistered per-layout action requires a separate audited adapter: KWin constructs translated per-layout action names and conditionally registers configured bindings during its layout reload. Generic enumeration must not create or guess such actions. That adapter remains on the backlog. Arbitrary command/application creation, scheme import/export, standard in-application shortcuts, and component removal likewise require separate adapters and are not exposed as fake native global-daemon capabilities.

## Native recording and preview

The Aero editor uses the installed KF6 XmlGui 6.24 `KKeySequenceWidget` with its
conflict checks disabled. The app's staged conflict/reassignment model remains
authoritative; it never calls the widget's steal/apply helpers. The owned
`KKeySequenceRecorder` binds to the current window and uses Wayland's scoped
keyboard-shortcuts-inhibit protocol during capture. Recording locks action and
alternative selection until completion so captured keys retain their intended
target. Completion, cancellation and destruction release the SDK recorder.
Offscreen tests send real Qt window key events for four chords, an occupied key,
plain keys and modifier-only sequences, and destroy a view during recording.
Actual compositor inhibition remains unverified; no live recording was tested.

This behavior was audited against the exact official
[KXmlGui 6.24 archive](https://download.kde.org/stable/frameworks/6.24/kxmlgui-6.24.0.tar.xz)
and [KGuiAddons recorder source](https://invent.kde.org/frameworks/kguiaddons/-/blob/v6.24.0/src/recorder/kkeysequencerecorder.cpp),
plus installed public headers. The control remains a native Qt widget styled by
Aero, with no embedded settings screen.

Keyboard Properties adds **Preview selected layout** for staged model, layout,
variant and options. It compiles trusted installed XKB data in-process with
libxkbcommon 1.13.1, without server access or preferences changes. The native
Aero dialog shows all compiled symbol groups and levels, modifier combinations,
and virtual/media keys. Local selection only highlights the preview.

Physical geometry for PC 101/102/104/105 comes from the installed
`geometry/pc` data, retaining its Open Group license notice. ANSI and ISO shapes,
Enter/Shift differences and key aliases are resolved against the compiled
keymap. Other recognized models display an accurate key list and an explicit
geometry limitation; their physical positions are not invented. Unknown saved
identifiers fail visibly. A single worker, copied requests and a UI deadline
bound dialog work and prevent stale callbacks after a dialog is destroyed.

The exact [Plasma Desktop 6.6.6 archive](https://download.kde.org/stable/plasma/6.6.6/plasma-desktop-6.6.6.tar.xz)
shows that Tastenbrett is an executable with private geometry objects, forces
xcb and uses XkbGetKeyboardByName with a process fallback. The native preview
uses its own Windows 7 rendering and does not launch or embed that program.
Arbitrary-model physical geometry and live keyboard behavior remain open.

## Recorded validation

On 2026-10-17, the complete build and **65 CTest suites passed in 28.96 seconds**.
The five focused shortcut/preview/keyboard suites also passed. Private D-Bus
fixtures exercise native serialization and persistence, read-only policy,
external file edits, rejected alternatives, partial writes and uncertain
outcomes. Session/UI fixtures cover Apply/Revert, explicit reassignment,
four-chord recording and work surviving page destruction. No live keyboard
recording or compositor inhibition was exercised.

Two read-only production snapshots returned identical registries with **277
actions across 22 components**, an available provider and writable native
configuration. SHA-256 fingerprints of `kglobalshortcutsrc`, `kxkbrc`,
`kcminputrc`, `plasmarc` and `kdeglobals` were unchanged. Discovery performed no
shortcut registration, invocation, preference write or reload notification.

The native Aero action tree, alternatives, conflict proposal, selected-layout
preview and keyboard layout page were visually inspected using fake-provider
offscreen captures. A second build reported no work; `cmake --install` passed
with `DESTDIR` confined to `build/batch13-validation/stage`. No live install
was performed. Logs, snapshots, captures and fingerprint comparisons are in
`build/batch13-validation`; source/evidence hashes are recorded in the parity
backlog. Live action activation, persistence across sign-in and full KDE
module acceptance remain unverified.
