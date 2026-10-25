# Batch 25: Power Options file jobs

Power Policy joins Screen Locking, Night Light and Day and Night in using
application-owned worker jobs and the shared settings-file reservation registry.
This batch preserves the existing 17 profile preferences and three low/critical
battery warning/action preferences. Charging-limit controls remain separate.

## Save lifecycle

Native configuration reads, provenance, typed/default handling, writes, sync,
failure cleanup, object destruction and readback belong to workers. Schema and
daemon process-environment file reads also leave the GUI thread. Values cross
the thread boundary; native configuration objects do not.

Read/write jobs retain application activity and daemon ownership after backend
destruction. Writes retain canonical/inode reservations for their target and
user kdeglobals through actual terminal verification. Old and replacement inode
aliases remain covered, including overlap with other participating providers.
Native KConfig still merges after acquiring its lock; this is not CAS or a
cross-file atomic transaction against other programs.

A soft deadline reports continuing work without freeing the job, permitting a
retry or declaring that native synchronization stopped. Before mutation,
cancellation can retire unsent work. During native sync it waits for the actual
worker result, preserves file ownership and avoids an unsent reload. Cancellation
after reload dispatch cannot retract a sent D-Bus request or prove no effect.

Owner/capability identity and full-source file receipts guard native reload
dispatch. Acknowledgement and saved readback remain separate observations from
physical effects and persistence. Ordinary self-generated reload/profile
notifications are distinct from provider invalidation. Slow-save progress stays
in the busy draft, and an explicit backend failure cannot be cleared merely by
matching readback. Outward controller signals tolerate session deletion.

## Charging preservation

On October 18, conservation mode was disabled and Fast charging selected. Battery
history showed an 80% cap through October 17 and charging above it from 08:37 on
October 18. Re-enabling Lenovo conservation mode and UPower's saved charging-limit
preference repaired the state; the original disabling actor remains unproved.

October 25 passive checks show conservation mode 1, Long_Life selected and saved
UPower preference 1 after a fresh boot. October 19–24 history stayed at or below
80%, with pending-charge observations near that cap. During this batch, passive
sysfs reads also showed 79%, Not charging and AC connected. The installed UPower 1.91.1
source reads a saved true preference to restore limits at startup; a missing
preference does not itself issue a disabling write. Saving the preference addresses reboot persistence; it does not identify what
originally selected Fast charging.

This batch records fresh fingerprints of those three charging-policy controls
alongside the monitored KDE files. Tests use fake daemons, private files and
private D-Bus sessions. No live power/profile/charging setter, native reload,
authorization, suspend or display action is part of validation.

## Validation

The Debug build succeeds. All **101 CTest suites pass in 191.94 seconds**;
the ten focused Power Policy, shared-file, navigation and lifecycle suites pass
in 22.84 seconds. All 410 frozen source/test/build inputs still match, and the
final Ninja invocation reports no work. A preceding invocation refreshed
unchanged MOC output without compiling or linking binaries.

Private fixtures cover GUI heartbeats during native lock waits, delayed success,
permission failure without destructor retry, cancellation before and during file
work, orphaned jobs, canonical and hardlink competition, shared user kdeglobals,
changed source receipts and provider identity, guarded native wire dispatch and
deletion from controller observers. The cross-provider suite includes 18 cases
across Screen Locking, Night Light, Day and Night and Power Policy. Unowned
charging-threshold entries in private fixtures remain preserved.

One fake-provider Aero capture was visually inspected. It asserts a changed
draft, continuing-save status and disabled Apply/Revert controls. Normal
and stripped DESTDIR installs each contain eight files, with approval manifests
matching the final helper and policy bytes. No live installation ran.

Across the work interval, **57 of 59** monitored byte fingerprints match. Cached
KNightTime state first differed during review, before the first build; the
KSycoca cache first differed during compilation. Both changes remain
unattributed, with private before/after bytes retained. All three charging-policy
fingerprints remain unchanged at every recorded checkpoint. These observations
do not exclude indirect effects or identify the October 18 disabling action.

The backlog preserves all 666 row IDs, statuses and earlier evidence, 151
implemented scopes and all 132 module/plugin inventory entries. No whole-module,
live-write or hardware-exclusion credit is added. Private execution evidence is
retained under `build/batch25-validation` and excluded from Git.

Full native KDE parity and real-session power effects/persistence remain open.
Appearance, Aero Effects, Aero Theme, Theme Components, Font and Keyboard still
need worker/shared-reservation migration; see the
[lifecycle audit](audits/kconfig-sync-lifecycle.md).
