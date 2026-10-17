# Batch 22: Aero login and shared lock appearance

The native Aero Login page covers the four options and picture chooser from the
installed custom AeroShell SDDM form. Search, category links, icon views and
Screen Locking reach it inside Control Panel. The only target is the approved
`sddm-theme-mod` theme. There is no alternate theme selector.

This is bounded implementation coverage. Host discovery verifies the fixed Aero
assets as root-owned and trusted; the new editor remains read-only because its
separately approved administrative helper is not installed under a protected
system prefix. Initial sandbox discovery presented those same files as
`nobody:nogroup`; a paired 13-path host/sandbox metadata check establishes the UID
mapping, rather than an ownership defect. Validation does not change ownership,
install to the live system, request authorization, restart a greeter or lock the
session.

## Native source and consumer behavior

The [source contract audit](audits/aero-login-source-contract.md) records the
custom KCM, AeroShell reader, SDDM and Qt baselines and their proof boundaries.
Observed installed binary hashes do not prove reproducible source equivalence.
The old custom KCM is not constructed as a passive reader: its KConfig `copyFrom`
path can dirty the base configuration merely when opening it. The old privileged
helper also accepts arbitrary caller paths and deletes before copying, so this
implementation does not reuse it.

| Owned preference | Source key | Expected effect |
| --- | --- | --- |
| Startup pearl animation | `enableStartup` | Login only; also depends on the greeter startup marker |
| Startup sound | `playSound` | Login only; the startup marker gates playback |
| Always show user selection | `forceUserSelect` | Login user-selection flow |
| Plain blue login background | `rdpBackground` | Coupled product action removes the shared picture override |

Actual SDDM reads base `theme.conf` and overlays only nonempty `.user` values
through QSettings INI. Qt boolean conversion treats `no` and `off` as true;
KConfig's native editor projection differs. Saved raw values, absent overrides,
empty user values and base defaults remain distinct. Production does not claim
to reproduce the native editor's unverified user-global cascade. New booleans
are canonical `true`/`false`; unknown stored values require explicit replacement.

Changing one login boolean leaves the image untouched. Choosing a local PNG or
JPEG prepares a bounded canonical PNG without administrative privileges, retains
the selected bytes across navigation and stages the necessary blue-background
companion. Removing the image is different from restoring it: without
`background`, Main.qml is expected to show plain blue, while the Aero lock reader
falls back to `default-background`. Restore Approved Image copies the exact
approved default bytes. Turning off blue cancels a pending removal; it cannot
recreate an image which was already absent.

Prepared PNG color space, pixels and resolution metadata are explicit. Qt GUI
and QCore processes otherwise choose different default DPI, which would make
independent canonical-image validation reject an unchanged picture.

Restore Approved Defaults is product functionality, rather than a native KCM
Defaults button. It reviews removal of the four owned user overrides plus an
explicit approved-image restore. Unknown/default values are not guessed, and
the theme selection, base file, foreign keys, comments and unrelated groups are
preserved.

The two picture thumbnails are static estimates. No QML, greeter, native KCM or
locker is launched for preview. The four login options do not add lock-screen
animation or sound settings. File readback proves storage, not rendering,
authentication, restart adoption or persistence across sign-in.

## Authorization and file transactions

The new fixed `controlpanel-aero-login-helper` accepts bounded strict JSON over
stdin, containing typed intents, image bytes and a fresh source revision. Caller
paths, arbitrary keys, duplicate JSON keys, alternate themes, stale revisions and
contradictory image companions are rejected. File/source revision and helper
identity are separate. Successful replies carry an exact post-commit revision;
the backend and controller both check fresh files against that receipt.

The active-session PolicyKit action requires `auth_admin`; inactive and other
sessions are denied. Helper capability inspection never executes it. The fixed
helper, policy, generated SHA-256 approval manifest and all ancestors must be
protected root-owned ordinary paths; the helper must not be setuid/setgid. CMake
regenerates the identity manifest when the helper or policy changes. A user-local
install prefix does not satisfy the production trust gate.
Configure the intended system prefix at build time so compiled helper paths and
the PolicyKit executable annotation agree; install-time relocation alone cannot
change that fixed contract. The installed approval is generated from the final
installed bytes after optional stripping or RPATH rewriting.

The filesystem core bounds plain-file reads and refuses symlinks, unsafe modes,
source aliases, unknown journals and foreign stages. It parses captured INI
syntax with every value neutralized before QSettings sees it; only safe owned
primitives receive scalar interpretation. This includes Qt's exact `#`, VT/FF,
BOM, quoted-line and escaped-key rules. Opaque QVariant payloads never reach a
root-side image or custom-type decoder.

Image validation uses a separate exec child which drops supplementary groups
and all user/group identities before loading the packaged image decoder. It has
memory/CPU limits, a wall deadline, closed inherited descriptors and a minimal
environment. Pipe EOF is followed by bounded process supervision. Only that
unprivileged decoder may be terminated on timeout; the privileged transaction
is never killed or automatically retried by the application.

A nonblocking fixed lock serializes cooperating writers. The helper rechecks
source identity after authorization and validation, writes a durable journal,
stages files, checks stage identity/content/mode and source/journal revisions at
each commit boundary, then uses fsync and per-file rename. Each file replacement
is atomic; there is no cross-file atomic visibility guarantee. Readback failure
attempts guarded rollback, retaining a journal when recovery is uncertain.
Recovery is explicit, reviewed and revision-pinned, and refuses foreign edits.
Its review identifies the restored boolean values, saved override provenance and
original background identity before requesting authorization.
No read silently rolls back or deletes staging files.

The journal is root-owned mode 0644 because passive clients must read the same
recovery evidence as the helper. Every original or staged source asset must
already be publicly readable; private files fail closed rather than having their
bytes exposed through the journal. A cleanup fsync failure can leave uncertain
durability after the journal was unlinked; the fresh snapshot then truthfully
has no current recoverable journal. Real OS-crash durability remains unverified.

## Persistent drafts and review

The app-owned session retains typed edits, image preparation, authorization and
recovery jobs across view destruction. A prepared but stale image remains an
explicit review candidate, counts as an unsaved change and blocks Apply until
reviewed. Clean typed refresh can update the baseline without discarding that
candidate. A failed Revert retains all selected bytes; only successful fresh
Revert clears them.

Page Apply, navigation Apply and Close Apply review the concrete changes and
expected login/lock projections. Acceptance is pinned to the reviewed saved and
staged state. Delayed authorization remains busy; a soft deadline does not imply
cancellation. Definite denial with unchanged files retains a retryable draft.
Partial writes, lost receipts, foreign changes and failed readback retain an
uncertain draft until explicit recovery and successful Revert. Closing is blocked
while work runs, and an unsaved candidate cannot silently disappear on Close.

## Validation

All **95 fixture suites pass (166.78 seconds)**, including the 13 focused
appearance/navigation suites (7.20 seconds). The final incremental build has no
work. Six native Aero captures were inspected: options, blue-login/default-lock
fallback, Apply review, verified saved files, an untrusted installation fixture,
and recovery review. Every capture uses fake settings/providers. Normal and
stripped eight-file DESTDIR installs both pass; each approval manifest matches
the final installed helper and policy bytes, with mode 0644.

Adversarial fixtures cover opaque QVariant decoder canaries, exact Qt key/BOM
semantics, stale source and helper identity, typed intent/schema forgery, image
bounds/canonicalization, source and staging races, modified modes/inodes/symlinks,
journal durability failure, partial commit/rollback, explicit and foreign-edited
recovery, lost receipts, canceled authorization, delayed jobs, failed Revert,
navigation/Close review and synchronous object destruction. Decoder supervision
also exercises a child which reports success and closes stdout but then sleeps.
Only unprivileged decoder/refusal modes run; no administrative helper operation
or authorization process is started by these fixtures.

Two matching passive host snapshots report four known booleans, trusted fixed
Aero assets, matching background/default hashes, no interrupted transaction and
no available new helper. All **56 monitored fingerprints and expanded inventory
paths match across the probe interval**. A file trace finds seven private
temporary INI opens and fourteen scratch link/unlink operations, with no
unexpected writable opens or path mutations. This describes the traced process
and does not exclude indirect effects. Paired host/sandbox metadata for thirteen
paths verifies the root-ID mapping described above; there was no ownership repair.

Across the full work interval, 47 final fingerprints match the initial 56-path
baseline. Nine differ: four service caches, shortcut and display configuration,
two Explorer desktop entries, and cached location state. Seven already differed
before the first build; two more first differed across the build interval, and
some earlier paths changed again. Private snapshots retain the bytes. Attribution
remains unresolved; no zero-change claim is inferred from the stable probe window.

No live preference setter, configure/Notify/reload, Lock/inhibition, acquisition,
registration/invocation, privileged helper operation, authentication prompt,
greeter launch/restart or live install ran. The release still needs trusted
system helper packaging, a disposable authorized write/recovery trial, rendered
login/lock acceptance and persistence verification. Full installed-KDE parity
remains incomplete. Artifacts are under `build/batch22-validation`.
