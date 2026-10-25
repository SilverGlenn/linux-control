# RC-2: Aero Theme and Keyboard native file jobs

Final integration update: the Release build passes all 107 isolated suites.
See the [qualification record](mvp-rc1-qualification.md) for final source/artifact
identity and installed status. Live effects, restoration and sign-in acceptance
remain unrun. The initial handoff notes below distinguish source work from tests
that had not yet run at that handoff.

Implementation package B, 25 October 2026. This note records source changes and
fixture intentions. Compilation, automated results and live acceptance remain
pending integration-owner validation; no test result is claimed here.

Aero Theme and Keyboard now keep a private application-owned coordinator behind
the public backend. Deleting a page, session or backend detaches its observers;
the coordinator retains runtime/transport ownership, application activity and file
reservations until its actual terminal result. Workers receive values and create,
use, clean and destroy native configuration objects on that same worker. File
reads, registry parsing, source fingerprints, preflight, synchronization and
readback no longer run in their GUI coordinators. Source receipts use bounded,
nonblocking regular-file descriptor reads with link/path/inode/metadata checks;
FIFO and dangling-link sources fail closed. Keyboard parses safely captured
registry bytes and fingerprints registry identities as well as their contents. File-system watcher registration
and runtime/reload coordination remain on the GUI thread.

| Provider | Native write targets | Read/conflict dependencies |
| --- | --- | --- |
| Aero Theme | User `kdeglobals`, `plasmarc`, `Kvantum/kvantum.kvconfig`, `kcminputrc`, `kwinrc`, `smodrc` | Each target's configured cascade; `kdeglobals`, `system.kdeglobals` and optional `/etc/kde5rc` global sources; approved installed assets; audited KWin/Plasma owners and effect interface |
| Keyboard | User `kcminputrc` hardware preferences, `kxkbrc` layout preferences, `plasmarc` OSD preference | Corresponding cascades; workspace FullConfig global sources and user `kdeglobals`; installed `evdev.xml` and optional `evdev.extras.xml`; pinned KWin/keyboard owners, version and layout interface |

Aero reserves all six user targets before mutation. Keyboard reserves all three
user targets and, for production FullConfig dependencies, user `kdeglobals`.
Inherited/system configuration participates in source receipts; read-only system
files and registry data do not become mutation targets. Isolated constructors
use explicit private paths and SimpleConfig, with injected runtime/transport and
asset probes. Aero does not edit wallpaper, fonts or shortcuts. Keyboard does not
edit the Mouse group even though it shares `kcminputrc` with Aero pointers.

Each native entry/key sync is a separate worker continuation. The GUI refreshes
the shared reservation with its resulting inode keys before the next replacement.
The registry retains original and intermediate inode aliases through subsequent
writes and final verification. Native failures clear dirty entries before native
object destruction and before any failure observer can restore permissions.
Aero retains its per-entry partial-save/no-rollback behavior; Keyboard retains
per-key Partial/Uncertain outcomes and its same-owner lockout after uncertainty.

The native deadline is soft: progress appears in the existing busy draft and
another Apply/Revert remains unavailable. Explicit cancellation retires unsent
work and waits for an active native worker. A running `sync()` is never declared
cancelled. Saved values remain after cancellation or provider replacement, with a
failed/partial/uncertain outcome and no unsent reload. Bounded runtime query/reload
timeouts are separate from the native worker deadline. Aero's existing runtime
timeout disables further mutation until application restart.

Aero production runtime discovery and reload use pinned unique KWin destinations
and a service epoch. The guarded reload rechecks job cancellation/provider state
after asynchronous owner discovery. Keyboard notifications target the audited
unique owner after fresh source/runtime checks. KConfig's native Notify remains
a broadcast; provider changes during a native wait cannot retract it. Shared
reservations coordinate this application's participants, provide neither an
external compare-and-swap boundary nor multi-file atomicity, and cannot prove
runtime adoption or sign-in persistence.

The public test boundaries preserve their existing constructors. Additional
isolated hooks are:

- `AeroThemeBackendOptions`: `beforeRead`, `beforeSync`, `afterSync`,
  `syncForTest(KConfig&)`, `softDeadlineMs`, `runtimeDeadlineMs`; passed through
  the additional absolute-directory/runtime/assets/options constructor.
- `KeyboardBackendOptions`: existing explicit paths/transport and budgets, plus
  `beforeRead`, `beforeSync`, `afterSync` and the existing `syncForTest`.
- `afterSync` runs on a worker after native object destruction and after capturing
  the post-save receipt. An external edit while this hook is held is detected
  before another write or explicit reload. Hooks must be thread-safe and cannot
  access GUI objects.
- `createKdeAeroThemeRuntime(explicitBus)` supports private-bus wire/owner fixtures.
  No test factory acquires a real session bus.
- Abstract backend `cancel()` defaults to no-op for existing fake backends;
  `operationPending` is additive and controllers accept progress only during the
  active write phase. Outward controller transitions tolerate observer deletion.

Focused cases were added to the existing Aero Theme/Keyboard backend and settings
suites: actual native lock wait with GUI heartbeat and delayed success, FIFO/broken-link
source refusal, same-byte source replacement guards, soft
progress, orphaned work/activity/reservations, cancellation and service replacement
before and during sync, foreign edits, intermediate hardlink aliases, failed sync
without destructor retry, private-bus owner/guard behavior, phase-scoped progress
and session deletion from observers. A failed terminal Keyboard read marks the
snapshot unavailable and unwritable while preserving failure context and drafts. Existing Aero tests now await asynchronous
completion. Package B owns the new cross-provider fixture; root owns CMake integration and
all build/test execution. The new private-process cross-provider fixture contains
14 bidirectional overlap cases, including both Keyboard hardware and workspace
files, and retains the existing Batch 25 fixture unchanged. Retired legacy AeroEffect and ThemeComponent writers are
unchanged.

Installed Aero tuning and keyboard repeat effect/restoration, application reopen
and later sign-in remain deliberate RC-4 acceptance work. Fixture success cannot
qualify those live effects.
