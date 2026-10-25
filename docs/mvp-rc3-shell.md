# RC-3 shell and availability implementation

Final integration update: the Release build passes all 107 isolated suites.
See the [qualification record](mvp-rc1-qualification.md) for final source/artifact
identity and installed status. Live effects, restoration and sign-in acceptance
remain unrun. The initial handoff notes below distinguish source work from tests
that had not yet run at that handoff.

Prepared 25 October 2026. **Implementation pending integrated validation; RC-3
and the release candidate are not yet qualified.** The integration owner runs
the build, automated suites and captures. This package has not run builds, tests,
setters, an installation, sign-out, reboot or live recovery.

## Changes

- A visible build identifier in the main-window title and an accessible `?`
  button beside search. The selectable About text uses Package A's compiled
  version, revision and source-manifest identity and starts no providers.
- Fixed candidate withholding reasons in `src/release/LocalRcPolicy.h`:
  `release::localRc::displayEditsWithheldReason()` and
  `release::localRc::connectivityEditsWithheldReason()` return `const char *`.
  There is no environment or user-facing enable switch.
- Display restriction is separate from actual helper availability.
  `DisplayBackend::editRestrictionReason()` defaults empty for injected fake
  sessions; the production implementation returns the fixed reason and rejects
  `beginPreview` before creating a helper process. The page retains discovery and
  disables all editing paths.
- Network restriction is separate from daemon checkpoint availability.
  `NetworkBackend::connectivityRestrictionReason()` defaults empty for fake
  sessions. The production concrete backend rejects connection actions and checks
  active-profile status again in its fresh preflight snapshot before mutation.
  Supported inactive profiles retain their existing CAS/update/readback path.
  Session and page gates expose the same reason; the overview also shows it.
- Existing network recovery fixtures explicitly choose `isolatedFixture` with
  their synthetic `org.example.*` service. Both are required. The real service
  name stays restricted even with that option. Recovery algorithms and guard
  timeout fixtures retain their existing scope.
- User-local helper limitations are explained in Internet Time and Aero Login.
  Root ownership, path, policy and byte-identity checks are unchanged; existing
  system-tool time actions keep their separate authorization path.

The exact candidate action list and W2 hardware prerequisite are recorded in
[known limitations](mvp-rc-known-limitations.md). Root owns integration, release
guidance and the final verdict. Existing Batch 25 changes remain part of the
candidate source.

## Regression coverage prepared

| Existing suite | Added checks |
| --- | --- |
| `main_window_lifecycle` | Compiled visible/selectable identity; Apply waits for delayed confirmed success before Home; failed Apply retains page/draft/error; external conflict blocks Apply navigation; reconstructed view retains a busy session and close waits |
| `display_settings` | Candidate reason remains distinct from actual rollback availability; staging and Apply stay unavailable; direct production preview fails before a helper process |
| `network_backend` | Restricted direct Activate/Disconnect/Wi-Fi/forged-empty-device active-profile update produces no mutation/checkpoint; inactive profile CAS still commits; real service name cannot take the fixture exemption |
| `network_settings` | Session dispatch rejects connectivity/active-profile staging and keeps inactive staging/preflight |
| `network_pages` | Connection buttons and active form disabled with useful reason; inactive draft remains editable/applicable; overview shows scope |

These are prepared checks, not pass claims. Existing navigation-guard,
shell-navigation, display guard, network recovery and helper trust suites must
also remain in the final isolated run. The navigation guard itself has not been
redesigned.

## Additional save-lifecycle ownership

Root subsequently assigned Default Apps and Standard Shortcuts' remaining RC-2
lifetime/reservation work to this package. Their existing file transactions now
run under app-owned job objects with independent watchers, background activity
and optional facade delivery. Deleting the backend/session removes unsent queued
work; an active worker and its GUI continuation still reach their actual terminal
result. Explicit `cancel()` drops unsent queued jobs and explains that active work
continues. A soft deadline reports progress while retaining busy state.

| Provider | Native writes | Reserved source dependencies |
| --- | --- | --- |
| Default Apps | User `mimeapps.list`, existing affected `<desktop>-mimeapps.list`, `filetypesrc`, `kdeglobals` terminal pair and `emaildefaults` profile/client fields, only when included in the prepared change | All association files across configured config/data precedence, and `kdeglobals`/`emaildefaults`/`filetypesrc` across user/system config directories; these are the same file set used for revision checks |
| Standard Shortcuts | User `kdeglobals` `[Shortcuts]` entries | User target plus inherited `/etc/kde5rc` when enabled, `system.kdeglobals` and system `kdeglobals` sources in native precedence; isolated fixtures supply explicit sources |

Default Apps also hashes installed desktop-entry inventory/identity before the
transaction and during final readback. Directory membership and outside writers
remain conflict checks, not an atomic desktop-inventory lock. No shared registry
can prevent arbitrary external KDE clients from racing.

Admission/refresh/release stay on the GUI thread; filesystem inspection and all
transaction-owned KConfig construction/use/destruction stay in workers. A narrow
blocking queued bridge sends only immutable identity keys to the app-owned job
object after **every** atomic replacement and owned rollback. The GUI never waits
for that worker, and the bridge receiver survives facade deletion. Refresh failure
stops further mutation with an uncertain/partial result. Original and intermediate
inode keys remain leased until terminal verification; Standard Shortcuts retains
them through its GUI-only native notification. Default Apps' multi-file rollback
uses the committed write's inode/timestamp receipt and preserves foreign
same-byte replacements.

Prepared focused checks in the existing Default Apps/Standard Shortcuts backend
and controller suites cover orphan reads/writes, delayed completion, queued
cancellation, post-replacement aliases, owned rollback after facade deletion,
notification on the GUI thread, overlapping `kdeglobals` writers, retained drafts
on failed Discard and controller deletion during outward signals. Root/B own the
integrated cross-provider fixture. None of these checks has been run by this
package; the final logs and results must be added by integration.

## Passive observations

Read-only `pactl -f json info/list sinks/list cards` found one already-default real
sink on PipeWire 1.6.2, no available headphone/HDMI alternative, and mute/0% volume.
The mandatory output-change portion of W2 is **blocked** until a supported real
second output is available. No live audio acceptance was performed.

Read-only `kscreen-doctor -j` found eDP-1, 1920 × 1080, scale 1, origin (0, 0).
This establishes the actual display-scale target, not layout acceptance.
Sanitized timestamped private records are under
`build/mvp-rc-review-20261025-start/{audio,display}-prerequisite.json`. These local
files are discovery evidence only; this document carries their shareable result.

## Outstanding evidence

1. Integration build and focused checks, followed by the complete isolated suite
   against the final candidate with retained identity/logs.
2. Fixture/native-scale primary-page captures and keyboard/focus/scroll checks;
   fix any observed clipped/unusable controls before claiming RC-3 complete.
3. Representative installed navigation and all mandatory live workflows,
   modified-provider effects/restoration, and persistence checks in RC-4.
4. Staged payload/runtime/rollback and final installed launch identity evidence
   from the release/integration packages.

Display and active-network installed recovery are not required to qualify a
candidate that keeps those optional mutations withheld. Their isolated recovery
tests remain required regression coverage and do not grant live qualification.
