# KConfig write lifecycle audit, 17 October 2026

The original audit found that Batch 21's deliberate native lock fixture blocked
the UI for approximately 45 seconds. KConfig 6.24 `src/core/kconfigini.cpp`
sets a 45 second lock wait; a UI-thread sync prevented the separate 30 second
QTimer from running. Batch23 moves Screen Locking's native file transaction to
workers with persistent ownership and soft progress deadlines. Its results and
remaining limits are tracked in [Batch23](../kde-batch23-save-lifecycle.md).

Batch 24 also migrates NightLight and NightTime and gives all three providers a
shared worker-inspected reservation facility. Batch 25 extends that facility
and persistent worker jobs to PowerPolicy. Remaining direct UI-thread callers
include Appearance, AeroEffect, AeroTheme, ThemeComponent, Font and Keyboard
backends.
DefaultApps and StandardShortcuts already use worker transactions and provide
useful local lifecycle examples. `KSharedConfig`'s installed SDK documentation
states that each thread has its own repository and inter-thread synchronization
is explicit: construct, use and destroy a transaction's config objects/groups
inside one worker rather than moving shared GUI-thread instances.

The Font and Keyboard failed-sync paths originally omitted `markAsClean()`.
KConfig's destructor retries dirty synchronization, and `reparseConfiguration()`
also synchronizes dirty objects. Emitting failure before destruction can
therefore precede another implicit write attempt. Batch23 adds markAsClean before
failure callbacks and private real-sync-failure fixtures that restore the
destination from the failure observer. Font's earlier XML commit remains an
explicit partial save. Both providers still synchronize on the GUI thread.

The first bounded migration is Screen Locking: worker-side fresh file
preflight, mutation, native synchronization, failure cleanup and file readback;
GUI-side service ownership/epoch checks, timers/watchers and configure
continuation. A persistent app-owned job and shared per-file reservation remain
until terminal completion, even if the view or backend disappears. A deadline
can say that saving continues; it must not declare final failure, kill/retry the
write or allow another mutation while native synchronization may still finish.

Required isolated acceptance cases include a heartbeat during lock contention,
delayed success, terminal lock failure without destructor retry, backend
destruction, a concurrent writer, foreign edits and owner replacement before
configure. Native `sync()` merges disk changes after acquiring its lock and has
no public compare-and-swap hook after that wait. Asynchronous execution improves
responsiveness; it does not establish stronger isolation.

Native KConfig Notify remains a broadcast. KConfig6.24 `sync()` can send Notify
after a native lock wait and even after a later write failure. Owner checks
before starting synchronization cannot cancel that existing native behavior;
provider changes or partial failures remain uncertain. Shared reservations
cover Screen Locking, Night Light, Day and Night and Power Policy, with the
remaining providers still needing adoption. See
[Batch24](../kde-batch24-shared-file-jobs.md) and
[Batch25](../kde-batch25-power-file-jobs.md).

Charging policy is separate from the three low/critical battery warning/action
keys in this editor. The October 18 incident found Lenovo conservation mode off
and no saved UPower charging-limit preference; the original disabling actor
remains unproved. The repair enabled conservation and saved UPower's preference.
October 25 passive checks confirm conservation on, Long_Life selected, saved
preference 1 and October 19–24 history at or below 80% with pending-charge states
near that cap. Batch 25 additionally fingerprints all three charging controls
before and after validation. Validation uses isolated files/fake providers and
never calls a live power/charging setter or reload.

Evidence inspected: repository backends and `tests/screen_lock_backend_test.cpp`,
installed `/usr/include/KF6/KConfigCore/ksharedconfig.h`, and cached official
KConfig 6.24 `kconfig.cpp`/`kconfigini.cpp` under `/tmp/batch18-power-audit`.
The original source audit ran no builds, tests, native setters, notifications or
service probes. Batch 23 records subsequent isolated validation separately.
