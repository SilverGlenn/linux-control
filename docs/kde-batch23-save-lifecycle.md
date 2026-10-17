# Batch 23: responsive Screen Locking saves and failed-sync cleanup

Screen Locking now runs its native configuration transaction in workers while
the application retains the job, service and file reservations. A soft deadline
reports that saving continues; it does not claim failure, cancel synchronization
or allow another write while the first transaction can still finish.

The Font and Keyboard providers explicitly discard unsaved KConfig dirtiness
before reporting failed synchronization. This prevents a destructor from
retrying the write after a failure observer restores the destination. Fontconfig
XML which committed earlier remains a partial save and is reported that way.

## File work and ownership

Screen Locking's fresh file preflight, typed native mutation, synchronization,
failed-sync cleanup, destruction, file readback and final configure file guard
run outside the GUI thread. Each worker constructs, uses and destroys its own
KConfig objects and groups. Snapshots, paths and options cross the boundary as
values. The GUI owns provider discovery, owner/epoch checks, callbacks, timers,
watchers and the actual asynchronous configure request. Installed package hash
and process-identity file inspection also use a value-only worker.

An application-owned write job reparents the service for the entire operation.
If the backend survives, it receives the service before terminal notification;
otherwise the job finishes independently. OperationManager activity keeps the
background-work strip and Close protection aware of the surviving transaction.
The reservation is per settings file and does not take the global package
transaction reservation.

The native target and, for the full KDE cascade, user kdeglobals are reserved
through file saving, configure and verification. Canonical paths and inode
aliases participate. Slow native synchronization retains the reservation and
the staged draft. Apply, Revert and editing remain unavailable until terminal
completion and the controller's readback. Progress messages are token- and
phase-checked and are not displayed as terminal errors.

Admission also inspects active jobs' current file identities in a worker, checks
their mutation epochs and the reservation inventory generation, and retains both
the original and replacement inodes. A native mutation in flight can cause a
conservative refusal when identity separation cannot yet be proved. This closes
the tracked-writer hardlink gap after KDE atomically replaces a file. The final
configure guard compares a separate full-source byte/identity receipt, including
foreign keys, in addition to the typed preferences.

An asynchronous file guard precedes configure. Its continuation rechecks the
pinned owner and epoch. Duplicate and late callbacks cannot send a second
configure request or revive a retired attempt.

## Evidence and remaining limits

All mutation fixtures use private files and fake providers. The Debug incremental
build succeeds and the final no-work build confirms current outputs. All 403
source/test/build inputs stay frozen during final validation. The nine focused
suites pass in 12.66 seconds. After correcting an optional screenshot fixture
which had selected an already-saved value, the final full 96-suite run passes in
169.12 seconds against the final frozen inputs.

| Acceptance case | Isolated evidence |
| --- | --- |
| Event-loop responsiveness | Held worker hooks and a real native QLock released by a GUI timer |
| Native terminal failure | Approximately 45 second lock timeout, heartbeat, soft pending and unchanged bytes after unlock/destruction |
| Persistent ownership | Deleted backend/session retains service, activity and Close protection until terminal completion |
| Reservation aliases | Same/dot/global paths, original inode and atomic replacement hardlinks, with surviving and orphaned backends |
| Final configure boundary | Foreign keys, defaults, policy, provider owner/epoch changes; duplicate, delayed and destroyed-service guards |
| Controller/navigation | Stale progress ignored; draft retained and editing/Apply/Revert blocked until completion and readback |
| Failed-sync cleanup | Real Font/Keyboard sync failures followed by restored destination; no implicit retry or reload; Font XML partial save retained |

Six final fake-provider Aero captures were inspected, including pending progress
with asserted busy/dirty state and disabled editing controls. Normal and stripped
DESTDIR installs each contain eight files; both approval manifests match the
final installed helper/policy bytes. The optional capture correction changes
only a test executable, leaving these production install inputs unchanged.

Two passive production snapshots match exactly: nine known fields, five writable
field capabilities and the audited KScreenLocker 6.6.5 provider. Both process/file
traces show zero writable opens or path mutations. Both probe intervals preserve
all 56 monitored fingerprints and all 56 expanded inventory paths. Tracing covers
the probe and its children, without excluding indirect effects.

Across the entire work interval, 55 of 56 final fingerprints match baseline.
Cached KNightTime state already differed before the first build and remains the
only observed drift. Private bytes preserve that evidence; attribution remains
unresolved. No settings are restored from those observations. Logs, input hashes,
captures, install records and passive snapshots are under
`build/batch23-validation/`.

This batch improves responsiveness and ownership. Native KConfig synchronization
still waits for its own lock and merges disk changes after acquiring it. There
is no public compare-and-swap hook under that lock. File reservations coordinate
tracked Screen Locking jobs; they cannot prevent arbitrary external writers or
make the KDE transaction cross-file atomic. Reservations currently coordinate this
provider's Screen Locking jobs; other providers have not adopted a common
cross-provider file reservation. A D-Bus timeout also cannot cancel a configure
request which has already been queued for execution.

Font and Keyboard synchronization remain synchronous. Other direct GUI-thread
KConfig writers still include Night Light, Day and Night, Power Policy,
Appearance, Aero Effects, Aero Theme and Theme Components. Their migration and
corresponding persistent-operation fixtures remain follow-up work; the
[lifecycle audit](audits/kconfig-sync-lifecycle.md) tracks the scope.

No live preference write, configure/reload, locking, authorization or helper
installation is part of this validation. Physical effects and persistence across
sign-in remain unverified. Full installed-KDE parity remains incomplete; this
batch does not add settings or declare a whole module complete.
