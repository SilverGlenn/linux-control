# Batch 24: shared KDE file jobs for Night Light and Day and Night

Night Light and Day and Night now run native file work outside the GUI thread.
Screen Locking, Night Light and Day and Night use the same settings-file
reservation registry, including their shared user kdeglobals file. Their saved
preferences, defaults, policy projections and existing Aero UI remain the same.

This is lifecycle work for existing controls. Full installed-KDE parity and real
preference adoption remain incomplete; this batch adds no whole-module or live
write completion claim.

## Native jobs and shared reservations

File preflight, typed native reads/writes, synchronization, dirty-state cleanup,
config/group destruction and file readback run in value-only workers. Each
worker constructs and destroys its own KConfig objects. App-owned jobs retain
service ownership, operation activity and reservations through actual terminal
verification, even after the requesting backend/session disappears. Read jobs
also retain application activity until their worker completes.

A soft deadline reports that saving continues. It never declares terminal
failure while a native write can still finish, cancels synchronization, retries
the write or releases its reservation. Controller progress is token- and
phase-checked, keeps the staged draft busy and does not masquerade as an error.
Night Light's outward state transitions also tolerate session deletion during
preview requests, preview cleanup, Apply, Revert and terminal notification.

The shared registry stays on the GUI thread; immutable inspection values and
shared atomic mutation epochs cross to workers. Canonical paths, existing inode
aliases, normalized missing paths and atomic replacement identities participate.
Admission checks registry generation, worker-observed epochs and retained plus
current file identities. Original and replacement inodes remain reserved until
terminal completion. Proven-disjoint existing regular files can proceed during
another native mutation; missing or ambiguous identities may conservatively
refuse admission. Package transactions retain their separate ownership.

These are cooperating application reservations, not filesystem locks against
other programs or a compare-and-swap transaction. KConfig still acquires its
native locks and merges disk entries after waiting. The remaining providers
have not adopted this shared facility yet.

## Preserved provider behavior

Night Light retains its four-key filter/default workflow, independent
temperatures, raw enum/default/global handling and bounded runtime observations.
Ordinary volatile property changes are separated from provider owner changes.
Its native schema reads use workers too. Stop, tracking expiry, owner replacement,
accepted Apply and destruction retire a preview's pending local schema check
before dispatch. Awaiting observers receive a bounded failure on retirement;
late replies cannot revive a retired lease. Reentrant newer requests retain
their own tracking. Destruction disconnects backend receivers before pinned
best-effort cleanup while retaining active job connections. Known-unsent work
does not trigger a global Stop. Native global preview ownership, transport
uncertainty and expiry after execution remain unchanged limitations: retiring
local tracking never proves remote cleanup.

Day and Night retains all seven preferences, separate editor and daemon views,
native time/coordinate/default semantics and cached-location provenance. Cached
location remains volatile evidence outside configuration conflicts. Installed
package hashes and existing process identity file reads also run in workers.
Passive reading never subscribes to or activates location or the daemon.

Both writers preserve native KConfig Notify behavior. Notify is a broadcast,
not an acknowledgement or a request pinned to one daemon owner. A provider
change observed before synchronization can abort the dirty write. A change
during a native lock wait cannot cancel the ongoing sync or its broadcast.
KConfig can even emit Notify after a later file-write failure. Partial saves and
provider changes therefore retain uncertainty rather than claim no effect or
physical adoption. No live Notify, preview/Stop or location action is part of
validation.

## Validation

The final Debug build and all **100 CTest suites pass (181.90
seconds)** against 409 frozen source/test/build inputs. The final 15-suite focused
run passes in 37.26 seconds, including shared admission/cross-provider jobs,
preview retirement/reentrancy, both controllers, asynchronous providers,
navigation and Close. The private native Screen Locking lock-wait regression
remains in the full suite. Final Ninja validation reports no work.

Two fake-provider Aero pending-save captures were inspected. They assert actual
staged differences, busy drafts, progress and disabled Apply/Revert. Normal and
stripped DESTDIR installs each contain eight files; helper/policy approval
manifests match the final staged bytes. No live installation ran.

Two passive native snapshots per provider match: Night Light reports four known
writable keys; Day and Night reports seven known writable editor keys and seven
known daemon inputs; Screen Locking reports nine known fields and five writable
preferences. All six process/child file traces contain zero writable opens or
path mutations, and all six probe intervals preserve every monitored fingerprint
and expanded inventory entry. These are getter observations, not runtime
adoption or write acceptance, and the traces do not exclude indirect effects.

Across the complete work interval, **54/56** final byte fingerprints match.
Cached KNightTime state already differed before the first build; stored display
brightness differed before the final build. Both changes remain unattributed,
with private before/after bytes preserved. No live setter, Notify/configure,
preview/Stop, location activation/acquisition, locking/inhibition, registration,
launch, helper authorization or live installation was used by validation.

The backlog preserves all 666 IDs, statuses and earlier evidence, 151 implemented
scopes and all 132 module/plugin inventory entries. There are zero whole-module,
live-write or hardware-exclusion claims. Private execution artifacts are retained
under `build/batch24-validation` and excluded from Git.

The [lifecycle audit](audits/kconfig-sync-lifecycle.md) tracks remaining GUI-thread
writers: Power Policy, Appearance, Aero Effects, Aero Theme, Theme Components,
Font and Keyboard. Their file work and shared ownership still need migration;
Font/Keyboard's failed-sync retry cleanup from Batch 23 remains in place.
