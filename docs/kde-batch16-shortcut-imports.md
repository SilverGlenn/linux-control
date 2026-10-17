# Batch 16: retained imports and complete application-component removal

This increment connects scheme command definitions to their later binding review
without losing the imported document when pages or dialogs are destroyed. It
also extends native application-component removal to additional and empty
contexts. Windows 7/Aero presentation and the approved-theme policy remain in
place. These are bounded shortcut capabilities; full installed-KDE parity and
real-session write/invocation acceptance remain open.

## Import lifecycle

`ShortcutImportCoordinator` is owned by the application and injected into views.
It retains bounded original bytes, their SHA-256 and source path, parsed commands
and bindings, explicit identity migrations and checkbox choices. Choices use
original identities, so mapping a command or rediscovering its registered action
does not reset unchecked rows. Export remains independent of this import state.

1. Review a scheme and select definitions and assignments. Unknown, unavailable
   and read-only assignments remain visible and can be explicitly excluded.
   A selected unresolved assignment blocks binding staging.
2. Stage selected command definitions atomically. Existing unregistered commands
   can combine definition editing and registration in the same draft. No native
   write occurs until the user chooses Apply on Custom Keyboard Shortcuts.
3. After a verified Custom Apply, queue read-only global discovery. Existing
   dirty, busy, conflicted or disconnected global sessions defer that read;
   their drafts are never silently reverted or overwritten.
4. Reopen the retained review, update it against the refreshed bindings, and
   explicitly stage selected global/standard assignments. Both complete drafts
   validate before either is committed. Apply remains on the settings pages.

The shared work strip offers Resume scheme import, including Ctrl+Shift+I.
Continue on Custom Keyboard Shortcuts closes the modal review before guarded
navigation. Closing a review suspends it. Discard review clears only the import
document and choices, preserving settings drafts and already applied values.
Control Panel resolves all dirty settings sessions before asking whether to
discard an unfinished import on exit. Review state is retained in memory during
the application session; it is not an on-disk restart queue.

Session epochs and applied/draft fingerprints invalidate stale reviews even
after a round-trip Revert. Generation guards reject queued callbacks for a
replaced/discarded import. Failed, partial, uncertain or unrelated Custom Apply
does not advance pending definitions. Session loss retains the source and
blocks staging. Completion requires actual applied values, registered matching
definitions and consistent global/custom native owners. A failed verification
keeps the draft pending even when the requested values appear saved.

## Context-neutral native removal

The exact source contract and trigger audit are documented in
[the custom-shortcut adapter](kde-batch15-custom-shortcuts.md#batch-16-whole-component-removal-and-command-adoption-audit).
Discovery validates the native read-only `uniqueName` property and retains the
complete context inventory, including entirely actionless components. Context
topology participates in snapshots, preflight and controller verification.

Default-only application services retain the native KCM `cleanUp` path. Other
components use audited context-qualified root `unregister` calls for distinct
action names; the daemon removes same-name siblings from every context. Native
persistence prunes the intended whole component. Entirely actionless components
use a checked empty dummy registration/unregister to schedule that pruning.
No context is activated, and no shortcut, application or command is invoked.

Each native step checks pinned owner, object paths, exact remaining actions,
context topology, files, policy and unaffected registry/configuration state.
The adapter requires complete registry and saved-group absence, with bounded
pruning/persistence waits. Immutable empty contexts, foreign edits, rejected
replies, timeouts and uncertainty remain explicit. These multi-step native
operations are not a distributed atomic transaction; partial outcomes preserve
the reviewed draft and require explicit recovery/reload.

## Command adoption and remaining work

The exact installed trigger chain retains a parsed service and reads its cached
Exec. A service-cache refresh alone does not replace an existing component's
service object. Native interfaces expose no per-component definition reload or
runtime Exec getter. Saving an edited definition therefore remains distinct
from proving its execution adoption; service/session restart or a separately
verified recreation adapter may be needed. This increment performs neither
restart nor cache rebuild automatically.

Arbitrary action deletion, context activation, additional physical keyboard
geometry, exhaustive dynamic-provider reconciliation, physical invocation,
recording inhibition and real Apply/sign-in persistence acceptance remain open.
The rest of the installed-KDE capability backlog is unchanged in scope.

## Verification

`shortcut_import_coordinator_test` injects fake providers for retained choices,
verified dependency reads, cross-scope staging, failures, owner changes, stale
callbacks and session destruction. Extended settings tests check combined
registration/edit, complete context readback and unverified-draft retention.
Native adapter fixtures use temporary catalogs/configuration and a private bus
for complete-context removal, pruning, wire signatures and file/registry races.
UI fixtures cover reopening, navigation, discard, export isolation and close
guards with native Aero controls.

The final Debug build passed all **72 CTest suites in 28.38 seconds**. All ten
focused shortcut/integration suites also passed. The additional combined native
backend edit-and-registration fixture passed in the full run. Retained,
pending-Custom-Apply and refreshed-binding review captures were inspected;
offscreen capture tests passed, including the existing broader window fixtures.
A no-work rebuild and DESTDIR install staging passed without a live install.

Read-only production discovery returns 205 applications, 12 registered
application components, 22 complete component/context inventories and 277 global
actions. Custom/global/standard repeated snapshots match; all 84 standard
bindings match the installed SDK, with zero mismatches or probe-specific
overrides. All 26 monitored live content/symlink fingerprints remain unchanged,
with no new application entries or service-cache files. Private pre-change bytes
for five configuration files are retained with the ignored validation artifacts.

Evidence is under `build/batch16-validation`: full/focused suite logs and detail,
fake-provider captures, probe snapshots/summary, fingerprint comparison and
staged-install/build logs. Validation performed no live settings write,
registration, cleanup, context activation, reload, invocation, command launch,
recording/inhibition or cache rebuild. Live effects and sign-in persistence
remain unverified; these fixtures do not establish full installed-KDE parity.
