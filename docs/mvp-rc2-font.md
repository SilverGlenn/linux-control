# RC-2 Font file jobs

Final integration update: the Release build passes all 107 isolated suites.
See the [qualification record](mvp-rc1-qualification.md) for final source/artifact
identity and installed status. Live effects, restoration and sign-in acceptance
remain unrun. The initial handoff notes below distinguish source work from tests
that had not yet run at that handoff.

Implementation evidence only; the final isolated suite and installed font-role
effect/restoration checks remain separate release gates.

`KdeFontBackend` now delegates discovery, reads, XML parsing, provenance checks,
native KConfig construction/sync/failure cleanup/destruction and saved readback
to value-only workers. The production constructor no longer invokes Fontconfig
or parses configuration through watcher discovery. `FcInitLoadConfig` parses
configuration only; no cache rebuild is requested. Workers create constant
fallback QFont values and do not query GUI application fonts, styles, platform
state or reload-service objects. Reload availability/identity is captured on the
GUI thread; reload dispatch and watcher registration remain there.

An application-owned job retains the reload service, operation activity and
shared file reservation until the actual terminal result, including when the
backend facade or its page is deleted. The soft deadline only reports pending
work. It neither completes nor cancels a native sync. A sent reload that times
out remains explicitly uncertain; it is not claimed to have been undone.

Reservations cover the chosen fontconfig XML, the KDE configuration write target,
the writable user `kdeglobals` when using the full cascade, and regular or missing
inherited KDE/fontconfig/additional source files. Initial aliases
and replacement inode identities remain owned through saved readback, reload
coordination and terminal verification. Fontconfig loaded files, include
directories, KDE inherited/global files, `system.kdeglobals` and applicable
`/etc/kde5rc` contribute provenance receipts. Include directories are **receipt
dependencies only**, never registry targets. Their membership is hashed without
trying to read them as XML. Source files have bounded regular-file reads and
identity/content receipts; unreadable, unsupported or drifting sources fail
closed. Read-only dependency receipts are checked again after synchronization.

The existing two-file save order remains XML first, KDE second. A failed second
step reports the partial XML save and requests explicit reconciliation; it does
not invent a rollback. On a failed isolated sync hook or native sync, the same
worker calls `markAsClean` before destroying its own KConfig instance, suppressing
the implicit destructor retry. Workers pass snapshots and requested values, not
GUI-owned native configuration objects. The XML commit is bound to the exact
temporary-file identity captured before its atomic commit. Before constructing
and again before synchronizing the second file, the worker checks the committed
XML receipt and all original non-XML source bytes/identities, including raw KDE
rendering mirrors. The original XML receipt remains guarded through a second-file
native lock wait, so a foreign same-value XML replacement is not adopted.

After each file replacement, the worker passes immutable identity keys through a
blocking queued bridge to the app-owned job's GUI registry. The job refreshes its
lease before the worker continues to the next file. A failed bridge stops further
mutation and reports the partial save; worker-owned configuration objects never
cross that bridge. The fixture-only `afterXml` hook exposes this intermediate
boundary to orphan/alias and source-drift checks.

Before an unsent refresh, the job verifies saved preferences, source receipt,
provider availability/identity and retained file identities. It repeats saved
receipt verification after reload completion. Font refresh remains a KDE
broadcast request: successful dispatch does not acknowledge adoption by running
applications. Application restart/sign-in caveats remain in the UI.

The controller retains a failed Revert's draft and reports that attempt's current
read error even after a prior uncertain Apply, scopes pending text to an active
phase and clears it on phase completion. Explicit failure/warning text survives
a matching saved readback and later refresh. It preserves the requested draft
and conflict after uncertain Apply. Outward state/apply signals are guarded
against receiver-triggered controller deletion. Recreated pages show pending
work and keep editing/Apply/Revert disabled until it actually ends.

Focused fixtures are `font_backend_test`, `font_settings_test` and the new
`font_async_test`. They exercise existing XML/policy/partial-save behavior,
worker-only sync failure and destructor cleanup, delayed reads, a real native
lock wait with a GUI heartbeat, soft deadlines, deleted facades with retained
services/activity, competing canonical and old/new inode aliases, source and
provider drift, include-directory membership receipts and phase/draft lifecycle.
Cross-provider `kdeglobals` cases belong to the integration owner's shared
fixture. These are private-file/fake-service checks; no live font, reload,
desktop setting or sign-in persistence claim follows from them.

Shared reservations coordinate this application's jobs. They do not make XML
and KConfig atomic together, prevent an external KDE client from writing, or
turn native sync into compare-and-swap. Installed acceptance must still change
and restore one supported font role, observe the consumer effect after any
documented restart, and record persistence as required by RC-4.

## Native font serialization

The first isolated run exposed a native readback mismatch for role-size changes:
the requested normal-weight font retained the `Regular` face label, while
KConfigGui deliberately saved it with an empty label. The exact
[KConfig v6.24.0 implementation](https://github.com/KDE/kconfig/blob/v6.24.0/src/gui/kconfiggroupgui.cpp#L153)
clears only `Regular`, `Normal`, `Book` and `Roman` when the weight is
`QFont::Normal`. KDE does this so consumers can select a real bold face rather
than synthetic emboldening; see [KDE bug 378523](https://bugs.kde.org/show_bug.cgi?id=378523).
The retrieved source file SHA-256 is
`484abe215a1ef648b0bd7967b6f4c13b13dbcca9584a9030d8d2953279af92e7`.

Controller selection/read ingestion and native reads/requests now use precisely
that stable representation. Family, point size, weight, italic/style and other
attributes remain intact. Other named faces and those same labels at other
weights are preserved and verified exactly. The ordinary preference equality and
all file/source receipts remain strict. Additional native fixtures cover all four
aliases, a specific normal-weight named face, and alias labels at bold/light
weights; the original async role-size and provenance assertions remain intact.
