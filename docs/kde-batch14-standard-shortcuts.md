# Batch 14: KDE standard application shortcuts

Audited target: installed KF6 Config **6.24.0**, Plasma Desktop **6.6.6**, Qt 6. The adapter supplies the common in-application bindings from KDE's standard shortcut catalog. The existing native Aero editor provides a separate persistent session for these settings and the global accelerator registry. Both use the same staged action, alternatives, conflict, Apply and Revert controller. This adds no theme choices or embedded KCM.

## Primary-source audit

- [KF6 Config v6.24.0 standard shortcuts](https://github.com/KDE/kconfig/blob/v6.24.0/src/gui/kstandardshortcut.cpp) and [public header](https://github.com/KDE/kconfig/blob/v6.24.0/src/gui/kstandardshortcut.h): enumerate every ID strictly between `AccelNone` and `StandardShortcutCount`, accepting entries with a nonempty native name, valid native category, and matching `findByName`. Labels, six category identities, and complete hardcoded defaults come from the installed SDK. There is no curated action whitelist. Native metadata/default getters execute on the GUI thread before immutable value capture into the worker.
- `initialize()` reads `[Shortcuts]` through the main application `KSharedConfig`, inheriting KDE globals. A missing key uses `hardcodedDefaultShortcut()`. The legacy string `none` disables an action. Other values use `QKeySequence::listFromString`, then remove duplicate alternatives while keeping first occurrence. This adapter reads the shared global source rather than any particular application's private overrides and never calls cached `shortcut()` getters for its snapshots or write verification.
- `saveShortcut()` writes to global `kdeglobals[Shortcuts]` with `Global | Persistent | Notify`. An ordered list exactly equal to hardcoded defaults uses `deleteEntry`, **not** `revertToDefault`. All other lists use `QKeySequence::listToString` in portable format, including an empty string for a disabled list. Staging uses KConfig with the same inherited default sources, so removing a default-valued binding retains KDE deletion-marker behavior when a system source defines that key. It does not merely remove the local text and accidentally expose the system override.
- [KF6 Config v6.24.0 configuration implementation](https://github.com/KDE/kconfig/blob/v6.24.0/src/core/kconfig.cpp): native globals merge `/etc/kde5rc` unless `KDE_SKIP_KDERC` is set, then `system.kdeglobals`, then `kdeglobals`, increasing specificity within each group of XDG directories. Immutable keys/groups/files stop their overrides. The adapter freshly enumerates these paths and parses them explicitly with `NoGlobals` and `addConfigSources`, avoiding KSharedConfig and the process-global path/value caches. Its revision includes source existence, complete bytes, permissions and file identities. Newly created sources and external policy edits participate in preflight.
- [KF6 Config v6.24.0 watcher](https://github.com/KDE/kconfig/blob/v6.24.0/src/core/kconfigwatcher.cpp) and [standard shortcut watcher](https://github.com/KDE/kconfig/blob/v6.24.0/src/gui/kstandardshortcutwatcher.cpp): native change notification is a D-Bus signal at `/kdeglobals`, interface `org.kde.kconfig.notify`, member `ConfigChanged`, carrying `QHash<QString,QByteArrayList>` (`a{saay}`) with group `Shortcuts` and changed native names. The adapter emits this exact notification after verified persistence. Applications opting into `shortcutWatcher()` reparse and update their cached standard values. This is a notification request, not acknowledgement that every application adopted the binding.
- [Plasma Desktop v6.6.6 standard shortcuts model](https://github.com/KDE/plasma-desktop/blob/v6.6.6/kcms/keys/standardshortcutsmodel.cpp): categories are File, Edit, Navigation, View, Settings and Help; displayed defaults are hardcoded SDK defaults; native saves call `saveShortcut` for changed entries. The adapter uses the same source identities, defaults and serialization, retaining ordered full alternative lists rather than reducing multichord sequences or the KCM's set representation.

Exact KF6 source copies used for this audit are `/tmp/batch14-kstandardshortcut.cpp` and `/tmp/batch14-kstandardshortcutwatcher.cpp`; the Plasma source is `/tmp/batch13-kde-source/plasma-desktop-6.6.6/kcms/keys/standardshortcutsmodel.cpp`. Installed headers independently match these APIs.

## Transaction and scope

`KdeStandardShortcutsBackend` implements `ShortcutsBackend`. An action's component is its native category ID, its context is `default`, and its action ID is the stable native standard-shortcut name. The owner identifies the native source path, and the interface signature identifies the source format and complete installed catalog. Shared conflict checking therefore covers all categories in the standard session. Existing collisions in native defaults remain usable; a newly introduced collision requires explicit staged release/reassignment. Standard bindings are evaluated separately from global shortcuts because their execution scopes differ.

Reads and writes run in a serial worker queue. Workers capture paths, catalog, baseline and requested alternatives by value, with no backend/UI pointers. Reads remain fresh after external file edits. Filesystem notifications watch both sources and their existing ancestors; explicit Refresh and Apply always reread the complete native state.

Apply validates the exact current baseline, native policy and staged final bindings. It prepares a private copy with KConfig, preserving unrelated keys, nested groups and unknown future shortcut entries semantically. It never opens live settings through a writable KConfig. A second complete preflight precedes one atomic file replacement. Parent directories are traversed with `O_NOFOLLOW`; symlinks, hardlinks, world-writable destinations and protected files are exposed as read-only. The transaction preserves existing permissions and checks file identity/content immediately before replacement. Native files can be normalized by KConfig, so comments and textual ordering are not guaranteed to remain byte-identical.

Fresh readback verifies every action, complete alternatives/chords, policy/defaults, and all inherited source identities. If verification fails while the file remains the transaction's own committed image, rollback restores the original bytes (or absence). An intervening external edit is preserved and reported partial. Notification failure leaves the verified saved bindings in place and explicitly reports a saved partial result. No runtime reload is sent before persistence/readback succeeds. File replacement plus D-Bus notification cannot be one distributed atomic operation; neither KDE nor this adapter supplies an application-adoption acknowledgement.

Only shared standard bindings are edited. Application-specific shortcut configurations are retained, unknown native keys are retained, and no global registration, accelerator setter, launch, invocation, keyboard-layout reload, cache rebuild, or KCM launch occurs. Applications may override the global defaults, cache them without watching for changes, or implement their own shortcuts. The user-facing completion must say **“Standard bindings saved. Applications may need to restart to adopt them.”** Stored native configuration is verified; shortcut invocation and universal application adoption are not claimed.

## Isolated verification

`StandardShortcutsBackendOptions` exposes absolute temporary config and explicit least-to-most-specific policy paths for tests. Worker race hooks and a fake typed notification callback operate only when `isolated` is true; isolated instances never use live user/system configs or the session bus. Tests cover installed catalog/default completeness, alternative ordering and four-chord retention, disable/reset serialization, inherited deletion markers, immutable entries/groups, unrelated settings preservation, cross-category collisions and explicit reassignment, stale-baseline/race guards, rollback, external-edit preservation, notification failure, unsafe links, and event-loop/lifetime behavior.

The separate `standard_shortcuts_backend_probe` only reads. It compares every current binding with `KStandardShortcut::shortcut(findByName(...))` in its fresh process and reports the compared count, mismatched IDs, and SDK values. It also detects existing application-specific config sources for the probe: those make the SDK comparison subject to private application overrides and are explicitly reported as a limitation. With no such source, zero mismatches independently verifies the installed SDK's effective global values against the adapter. This process never writes, notifies, registers, invokes, records keys, or launches applications.

Build and execution results are recorded by the integrating root task. No live settings writes or runtime notifications are part of this batch's validation.

## Recorded integration validation

On 2026-10-17, the complete Debug build and **66 CTest suites passed in 32.00
seconds**. The standard backend's twelve isolated cases passed, as did the
shared controller, both scoped pages, catalog routing and navigation/job
lifetime fixtures. The native Aero standard category tree and unchanged
global tree were visually inspected from offscreen fake-provider captures.

Two production read-only snapshots returned identical values for **84 actions
across six categories**. All 84 matched the installed SDK, with **zero
mismatches** and no application-specific probe configuration source. The
provider was available and writable outside the filesystem sandbox. No native
save, notification, invocation or recording was performed by the probes.

SHA-256 fingerprints of five relevant live configuration files remained
unchanged. A second build reported no work and the install passed with
`DESTDIR` confined to `build/batch14-validation/stage`. Logs, snapshots,
fingerprint comparisons and captures are in `build/batch14-validation`;
source/evidence hashes are recorded in the capability backlog. No live install
was performed. Saved binding changes, application adoption and persistence
across sign-in remain unverified on the real session.
