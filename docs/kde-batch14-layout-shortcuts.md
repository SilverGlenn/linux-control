# Batch 14: KWin per-layout shortcut creation and activation

This optional adapter targets the exact installed combination KWin **6.6.6**, kglobalacceld **6.6.5**, and KF6 KGlobalAccel/KConfig **6.24.0**, on KDE Wayland. Ordinary global shortcut discovery remains read-only and available when the optional layout capability is absent. Neither probing nor page creation registers actions, reloads KWin, switches layouts, invokes shortcuts, or writes preferences.

## Audited native protocol

- [KWin v6.6.6 keyboard_layout.cpp](https://github.com/KDE/kwin/blob/v6.6.6/src/keyboard_layout.cpp): `getLayoutsList()` exports `a(sss)` with short name, display name, and the **already translated** xkeyboard-config long name. `loadShortcuts()` constructs the identity `Switch keyboard layout to ` plus that same translated long name. It creates an invocation-owning QAction only when the native assignment is nonempty. This explains why changing a previously disabled registration without reloading KWin cannot prove activation.
- [KWin v6.6.6 keyboard_layout.h](https://github.com/KDE/kwin/blob/v6.6.6/src/keyboard_layout.h) defines `/Layouts`, `org.kde.KeyboardLayouts`, typed `getLayoutsList`, and the zero-argument `layoutListChanged` signal. KWin registers both `org.kde.KWin` and `org.kde.keyboard`; the adapter requires the same unique bus owner for both.
- [KWin v6.6.6 main.cpp](https://github.com/KDE/kwin/blob/v6.6.6/src/main.cpp) opens `kxkbrc` with `KConfig::NoGlobals`. KeyboardLayout watches its `Layout` group. A notification reloads the configuration, calls `loadShortcuts()`, then unconditionally emits `layoutsReconfigured`, forwarded to `layoutListChanged`. Thus that owner-pinned signal is an observable completion of the callback rebuilding path. It is not proof of a physical key invocation.
- [KF6 KConfig v6.24.0 watcher](https://github.com/KDE/kconfig/blob/v6.24.0/src/core/kconfigwatcher.cpp) subscribes to `/kxkbrc`, `org.kde.kconfig.notify`, `ConfigChanged`, with `QHash<QString,QByteArrayList>` (`a{saay}`). The adapter sends this native signal **only to the verified KWin unique owner**, with `Layout -> [LayoutList]`. No `kxkbrc` value is changed to manufacture a notification.
- [KF6 KGlobalAccel v6.24.0 configuration actions](https://github.com/KDE/kglobalaccel/blob/v6.24.0/src/kglobalaccel.cpp) register via `doRegister(as)` and omit `SetPresent` for `isConfigurationAction`. Configuration actions do not invoke callbacks. [Plasma Desktop v6.6.6 keyboard bindings](https://github.com/KDE/plasma-desktop/blob/v6.6.6/kcms/keyboard/bindings.cpp) use this configuration-action mechanism for per-layout shortcut editing. The adapter uses the underlying asynchronous protocol without creating a local QAction or registering any invocation subscription.
- [kglobalacceld v6.6.5](https://github.com/KDE/kglobalacceld/blob/v6.6.5/src/kglobalacceld.cpp): `doRegister` creates a missing action; `setForeignShortcutKeys(as,a(ai))` applies `NoAutoloading`, preserves defaults/presence, broadcasts the assignment change, and schedules durable native persistence. Existing friendly metadata is updated only for nonempty supplied friendly fields. The adapter supplies **empty friendly fields** during registration and assignment, and reads back all native identity/default/context metadata before assigning keys. It never sets defaults or presence, and never cleans, unregisters, steals, or invokes actions.

## Capability and shared staging

The adapter verifies KWin's introspected `supportInformation() -> s`, exact version 6.6.6, the typed layout list and reload signal, and the daemon's `doRegister(as) -> void`. It derives action identities only from runtime translated long names. Missing/duplicate/invalid runtime names, mismatched owners, unsupported versions, unavailable registration methods, or a missing existing keyboard-layout default context disable this adapter with a reason. Ordinary globals remain editable.

`ShortcutsSnapshot.layouts` records the pinned compositor owner/version, runtime action identities, and a revision covering all typed runtime layout metadata and the saved/cascading `kxkbrc` file baseline. Existing per-layout registrations use `layoutAction=true`; missing approved runtime actions are synthesized with `registered=false`, empty current/default bindings, and native component/context labels. A blank action-friendly label intentionally lets the UI show the exact action identity without overwriting any native metadata. Historical or guessed layout identities are not creation targets.

Synthetic and existing actions share the ordinary shortcut draft, collision validation, explicit reassignment, Apply/Revert, and navigation lifecycle. Clearing an unregistered empty candidate performs no registration. Clearing an existing binding saves an empty native assignment and reloads the callback list while preserving its registration and defaults.

## Apply and observed completion

Apply re-reads the daemon registry and capability baseline before mutation. It uses all three native collision modes, releases explicitly removed alternatives first, and registers only requested **nonempty** missing actions whose exact identities remain in the verified runtime list. Registration is followed by a complete native metadata/default/current readback before any binding is assigned. Changes of daemon/compositor ownership, runtime layouts, version, or keyboard/native config stop further mutation.

The adapter preserves the existing accepted-key and durable native configuration readback. After the desired bindings have persisted, it subscribes to the verified owner's `layoutListChanged`, sends the targeted native `Layout` notification, waits for that unique sender's acknowledgement, and re-reads the complete capability and native assignment state. Only that combined result yields Success. A registration error or an outstanding timed-out registration/reload produces Uncertain and locks further Apply to that daemon owner. A known reload failure after durable binding save yields Partial and explicitly reports saved bindings with unverified activation. There is no speculative fallback, direct layout switch, key injection, KCM launch, or live validation mutation.

KDE provides no atomic registration/assignment transaction, compare-and-swap, callback enumeration, or physical invocation proof. Read guards and complete readback detect partial results; the acknowledged KWin reload establishes that the audited callback rebuild path completed for the same owner and unchanged runtime layout list. Failed partial registration is left visible rather than hidden by destructive cleanup.

## Isolated verification

The fake transport fixtures cover translated runtime names, read-only synthetic discovery, successful missing-action registration/persistence/reload, existing disabled action activation, empty removal, preserved native metadata/defaults, unknown/duplicate layouts, unsupported versions with ordinary global writes, saved config changes, KWin restart before/during registration, registration timeout/error, reload partial/uncertain outcomes, and explicit reassignment shared with an ordinary third-party action.

A private temporary D-Bus fixture exercises the **actual** typed `a(sss)` decoder, targeted `/kxkbrc` `a{saay}` notification, expected group/key payload, unique sender acknowledgement, no notification delivery to an unrelated connection, and rejection of a matching-path/interface acknowledgement sent by a different unique owner. The latter reaches the bounded uncertain timeout. All writers, registrations, reload receivers, and config files in tests are fake/private/temporary. Root alone builds and runs validation. No live session registration, reload, invocation, shortcut recording, or preference write is performed for this batch.

## Recorded integration validation

On 2026-10-17, the complete Debug build and **66 CTest suites passed in 32.00
seconds**, including the layout registration/reload fixtures and the actual
private-bus notification/acknowledgement checks. Shared controller tests reject
false successful registration or changed compositor identity. Page and
navigation fixtures retain independent drafts and jobs after views disappear.

Two production read-only probes returned identical snapshots: **277 global
actions across 22 components**, writable native configuration, and an available
KWin **6.6.6** layout adapter. The adapter resolves the configured **English
(Colemak)** runtime name to its already registered, currently empty per-layout
binding. No action was newly registered or assigned, and no live reload or
invocation was requested.

Five relevant live configuration files had unchanged SHA-256 fingerprints.
Native Aero global/standard shortcut captures were inspected. The repeated
build reported no work, and install staging passed under
`build/batch14-validation/stage` with no live install. Evidence is retained in
`build/batch14-validation` and fingerprinted in the capability backlog. These
observations establish read capability and isolated protocol behavior; real
shortcut activation and persistence across sign-in remain unverified.
