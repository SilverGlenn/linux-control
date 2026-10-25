# Local MVP release candidate

Decision and final handoff review: **25 October 2026**.
Status: **A–C preparation passed; candidate installed for acceptance; RC qualification remains open**.

The [repository review and assignments](mvp-rc1-review.md) record prerequisite
gaps and corrections. W2 currently requires a second supported physical playback
output: the October 25 inventory exposes only one, already selected. W2 remains
mandatory and blocked until that change can be demonstrated; implementation of
A–C can proceed independently.

## Release decision

The next milestone is a versioned Control Panel installed on this machine and
useful enough for daily use to generate concrete feedback. Freeze new settings
categories until that trial has happened. Preserve the native Windows 7/Aero
interface and the existing approved-theme policy.

The first target is this Ubuntu 26.04.1 KDE Wayland installation, its supported
provider versions, and its installed custom Aero assets. Broader distribution
support and full KDE parity are later milestones. The
[parity backlog](kde-parity-status.md) remains valid, but completing its 666
overlapping capability/obligation rows is not an MVP release gate.

An RC qualifies a bounded set of everyday workflows. It does not declare every
existing editor fully validated. Keep useful existing features and their honest
limitations; fix or disable a known unsafe action before shipping it. Do not
expand an editor solely to satisfy full-module parity during this milestone.

Three milestones distinguish preparation from acceptance:

| Milestone | Required result |
| --- | --- |
| Ready to install for acceptance | RC-1, RC-2 and RC-3 pass, with the final isolated suite and staged-install evidence; live acceptance is still pending |
| Qualified local RC1 | Install that exact artifact and pass RC-4; supply the feedback materials in RC-5 |
| Daily-use review | Gather feedback over five working days and prioritize the next candidate; this period does not delay the first installation |

The seven everyday workflows below are mandatory. Disabling a failed mandatory
workflow does not make the candidate pass. Any proposed reduction of that scope
must be recorded as a product decision, rather than silently checked off by an
implementation worker. Optional actions can be withheld as described below.

## Current state and evidence

| Area | Observed state | Implication |
| --- | --- | --- |
| Product | Native settings pages, search/category/icon navigation, persistent drafts, conflict handling and background jobs already exist | Enough functionality exists to start qualifying everyday use |
| Automated checks | The saved October 25 run passed 101 suites with zero failures; all 410 source/test/build input hashes still matched during this review | Strong fixture baseline; this review did not rerun the suites or verify real setting changes |
| Build | Existing CMake cache is Debug, configured for `/home/siru/.local`, with external AeroQt | A fresh Release build and installed-runtime check are still required |
| Install staging | Batch 25 staged eight installed files, both normal and stripped; helper approval manifests matched final installed bytes | Packaging mechanics have evidence, but staging is not a working local installation |
| Existing installation | `control` resolves to `/home/siru/.local/bin/control`, dated October 16; its desktop entry is also older; the three current helpers are absent under `.local/libexec` | The launcher currently reaches an older application, not the validated build |
| Save lifecycle | Screen Locking, Night Light, Day and Night, and Power Policy use persistent workers and shared file reservations | Reuse this pattern for remaining active blocking writers |
| Live acceptance | Existing audits explicitly leave live effects and sign-in persistence unverified | Fixtures and successful readback cannot substitute for desktop use |
| Release guidance | README installation guidance emphasizes Arch; the project has no version in CMake or `main.cpp`, and no release identity UI was found in the shell | Installation and feedback need a clear identity and instructions for this target |

This is the October 25 starting snapshot, not a permanent claim about later
checkouts or installations. Record fresh build/provider identities during
qualification. A changed provider version must satisfy the existing capability
guards; do not weaken them to make an acceptance scenario pass.

Evidence: [Batch 25](kde-batch25-power-file-jobs.md),
[save lifecycle audit](audits/kconfig-sync-lifecycle.md),
[CMake install rules](../CMakeLists.txt), [application entry](../src/main.cpp),
and local files under `build/batch25-validation`. The build evidence is ignored
by Git. Existing uncommitted Batch 25 changes are part of this assessment and
must be included in the candidate's source identity; HEAD alone is insufficient.

## Required everyday workflows

These are the minimum acceptance scenarios, using capabilities this machine
actually exposes. Each requires an observed outcome, not just an enabled button.

| ID | Workflow | RC acceptance |
| --- | --- | --- |
| W1 | Open and find settings | Launch from the desktop menu; find Sound, Mouse/Touchpad, Desktop Background, Night Light, Power Options and Personalization through search; use Back/Home and keyboard navigation without losing pending work |
| W2 | Sound | Change playback volume/mute and a supported default output; observe the result and restore the baseline; reopening the dialog shows the actual state |
| W3 | Mouse/touchpad | Stage one supported pointer or scrolling preference, discard it, then apply it; verify physical behavior and restore it; retain usable input throughout |
| W4 | Desktop background | Apply a local picture and fill mode to the selected desktop; verify the visible result and restoration; other desktops retain their preferences |
| W5 | Night Light | Preview and stop; apply one temperature/enablement change; verify the screen and saved state; preserve unrelated schedule settings |
| W6 | Power Options | Edit one supported timeout on the applicable power source; verify saved values, effective behavior and restoration; preserve battery charging-limit policy |
| W7 | Aero personalization | Apply one supported tuning value, verify its visible effect and restore it; preserve approved component identities and unrelated settings |

For persistent preferences, verify application reopen and a later sign-in or
reboot. Temporary preview and application-stream audio controls have different
lifetimes and must not be treated as persistent preferences. Any restart needed
for adoption must be explained accurately in the UI or known limitations.
Check the changed value before restoring it: capture baseline, apply, observe,
reopen, check persistence where applicable, then restore and verify the baseline.
Several scenarios may share one coordinated sign-in/reboot. Merely restoring the
baseline before restarting does not prove the changed value persisted.

Other existing pages remain useful feedback surfaces. Display and active-network
edits require their own Keep/Revert/timeout recovery smoke checks before those
actions qualify for the RC; leave those edits unavailable in the candidate if
recovery cannot be established. The user can continue using KDE's existing tools
for capabilities outside this candidate's supported scope.

List which optional actions are enabled and which are withheld in the candidate's
known limitations. A withheld action must reject mutation at the operation/backend
boundary as well as in its controls; search, sidebar links, restored navigation
and direct dispatch must not bypass it. Do not build a general feature-flag system
for this requirement. Unchanged secondary editors need a launch/read smoke check
and their existing regression coverage; exhaustive live parity is out of scope.

## Release gates

### RC-1: Identify, build and prepare a reversible install

- [x] Add a visible local RC/build identifier and a command-line version result.
  The version command must work without a graphical session or starting provider
  operations. Record the source revision plus a content manifest or source archive
  covering included modified and untracked inputs; a generic `dirty` suffix alone
  cannot identify a candidate. Record hashes of the distributed binaries.
- [x] Produce a fresh Release build for the final install prefix. Record required
  Qt/KF6/KNightTime/AeroQt dependencies and the supported provider versions;
  preserve existing compatibility guards. Use a separate build directory and
  configure the absolute prefix `/home/siru/.local` before compilation: helper
  paths are compiled in, so changing only the install command's prefix is insufficient.
  Include Qt Positioning, KF6 Holidays, CUPS, fontconfig, xkbcommon, OpenSSL and
  zlib from the actual CMake requirements. Distinguish development inputs from
  runtime dependencies: the existing cache uses extracted development files under
  `build/deps` for Positioning, KNightTime, Holidays and CUPS. Record those inputs'
  provenance/hashes or use matching installed development packages; installed
  runtime libraries alone do not make a fresh configure reproducible.
- [x] Document and exercise user-local installation, upgrade and removal. Back up
  existing managed application files before replacement and retain a manifest so
  rollback/removal touches only those files, not the user's KDE preferences.
  Test in a staging directory: preserve pre-existing bytes/symlinks, remove only
  candidate-created files, and report intervening edits instead of overwriting
  them. This is application-file rollback, not rollback of settings changed in use.
- [x] Stage the desktop entry, application and display guard consistently.
  Verify AeroQt and other libraries resolve without the source/build directory
  or development-only environment variables. Prepare the launcher so the actual
  menu/terminal identity check in RC-4 can distinguish it from the older executable.
  Generate the installed desktop entry with the configured absolute executable
  path so menu resolution does not depend on an older PATH entry. Replace the
  old single-binary copy instructions for this candidate's multi-file payload.
- [x] Keep privileged time-server and login-theme editing unavailable in a
  user-local install. Their root-owned helpers and system polkit installation
  are a separate follow-up, not a dependency of first daily use. Explain this
  limitation rather than presenting an authorization path that cannot work.
- [x] Verify the staged payload, runtime dependencies and rollback before touching
  the existing installation. Record the final payload rather than assuming it
  always has eight files. Actual installed launch/sign-in checks belong to RC-4.

The existing CMake payload includes both privileged helpers, policies and the
login-helper identity manifest. They may remain in the user-local payload, with
their existing ownership/trust checks intact; copying policies under `.local`
does not authorize them. Do not install them system-wide for this milestone.
Treat the already-installed AeroQt/theme as dependencies: do not take ownership
of, remove or upgrade those shared assets as part of application rollback.

The first distribution artifact may be a local staged bundle with its manifest
and install instructions. A public release, AUR update, Debian package, universal
installer and automatic updater are not required.

The owner chose **manual updates through System and Security > Linux Update**,
then cancelled Cloud migration. Development and builds stay on the laptop.
That follow-up is specified in [the update workflow](manual-updates.md), with
[the local development workflow](local-development.md). It does not change this
candidate's mandatory workflows, qualification status or external dependency
ownership. Initial laptop bootstrap and real release verification remain separate
from source implementation and fixture results.

### RC-2: Finish the active save lifecycle

- [x] Move native file transactions in Aero Theme, Font, Keyboard and Appearance
  (the title-bar action) off the GUI thread. Keep ownership and busy state until
  real completion, including after page replacement or a soft deadline.
  Construct, use and destroy transaction-owned KConfig objects in the same worker;
  pass values across threads, not GUI-owned configuration objects. Keep GUI objects
  and service-owner/reload coordination on their owning thread.
  Include reads, preflight, source/default discovery, failure cleanup, destruction
  and readback. Fontconfig discovery currently happens during construction and
  watching as well as reading; it must not remain a blocking GUI-thread path.
- [x] Coordinate overlapping files with the existing shared reservation registry.
  Include Default Apps and Standard Shortcuts, which already use workers but
  also write `kdeglobals` and currently do not participate in that registry.
  Inventory each migrated provider's actual write targets and inherited/global
  dependencies. Reserve the transaction's relevant files before mutation, retain
  replaced inode aliases through verification and any owned rollback, and release
  only at terminal completion. Preserve the existing multi-file partial-save rules.
  Default Apps and Standard Shortcuts also need application-owned mutation jobs:
  their existing value-only workers survive backend deletion, but activity and
  GUI-side reservation/reload/rollback continuations currently do not. Retain all
  original, intermediate and replacement inode aliases, including owned rollback.
- [x] Preserve drafts and report denied, failed, partial and uncertain outcomes.
  Do not retry implicitly after reporting failure, claim cancellation of an
  already-running native sync, or permit another conflicting write too early.
- [x] Verify lock contention with a responsive GUI heartbeat, delayed completion,
  destruction/navigation, external edits, service replacement and overlapping
  writes using isolated fixtures. Preserve unrelated settings on success/failure.
  Cover at least Aero Theme versus Night Light/Appearance on `kwinrc`, and Font,
  Default Apps and Standard Shortcuts against another participant on `kdeglobals`.
  Include Aero Theme/Keyboard overlap on `kcminputrc` or `plasmarc`. Latch provider
  invalidation during workers and guard unsent reload dispatch; native Notify
  broadcasts and already-sent calls retain their documented uncertainty.

The audit lists six direct-sync adapters, but two are already production-retired:
`KdeAeroEffectBackend::writeStrength` and
`KdeThemeComponentBackend::writeComponents` reject non-isolated writes.
The live Aero Effects route and component restoration use the shared Aero Theme
session. Keep those rejection paths covered; migrating their test-only writers
is not an RC prerequisite. Sources:
[Aero Effects](../src/platform/kde/AeroEffectBackend.cpp),
[Theme Components](../src/platform/kde/ThemeComponentBackend.cpp),
[shell routing](../src/ui/MainWindow.cpp).

Shared reservations coordinate this application's participants. They do not
make multi-file updates atomic or prevent external KDE clients from racing.
Retain the existing explicit conflict and uncertainty behavior.

### RC-3: Make supported behavior clear

- [x] Exercise Apply/Discard/Cancel, external-edit conflict and returning to an
  active job through the shell with injected backends before installation; repeat
  representative navigation during RC-4. A failure must keep the draft and explain
  what happened; closing must not silently abandon an active mutation. Retain
  Sound's explicit immediate-action behavior rather than adding a false Apply/Revert.
- [x] Keep unavailable providers, missing hardware, unsupported versions and
  unfinished actions visibly unavailable with a useful reason. Do not report
  successful updates or security status when the underlying information is absent.
- [x] Make Ubuntu package-management limitations and privileged-helper limits
  prominent in installation/known-limitations guidance. Preserve clearly labelled
  existing development bridges without claiming native parity.
- [x] Check the primary pages at this machine's actual display scale: readable
  fields and errors, visible action buttons, sensible focus and keyboard use.
  Fix clipped or unusable controls; defer cosmetic refinements that do not block use.

### RC-4: Qualify the installed application

- [x] Run the complete isolated suite against the final candidate build. Add
  focused regression coverage for changes in RC-1 through RC-3; retain logs and
  source/build identity. Current baseline: 101 suites, not a fixed target count;
  do not drop or silently skip existing suites to obtain a green result.
- [ ] Install the tested payload and record its manifest/hashes. Confirm the
  desktop menu and terminal open the same version, from an unrelated working
  directory and after a fresh sign-in. The installed app must resolve its helpers
  and libraries independently of the checkout. No system-wide install is required.
- [ ] Complete the everyday workflow table on the installed executable. Record
  baseline, requested change, observed effect, saved/read-back state, restoration
  and persistence result. A fixture or notification alone does not pass a row.
- [ ] Live-check one representative change and restoration for each modified
  provider in RC-2. W7 covers Aero Theme; additionally check a font role, keyboard
  repeat preference, title-bar action, default terminal (the `kdeglobals` path),
  and a common application shortcut. Observe each supported effect, allowing a
  documented consumer restart, without claiming all controls in that editor pass.
- [ ] Keep charging-limit preferences outside Power Options mutations and record
  their before/after state during power acceptance. Prior incident attribution
  remains unresolved; do not infer that tests proved no indirect effects.
- [ ] Record failures and unresolved limits next to their scenarios. No known
  configuration loss, wrong-target write, unrecoverable display/network change,
  repeatable crash, indefinite blocked UI or broken launch may remain in any
  reachable enabled action. Fix a mandatory workflow; fix or withhold an optional
  action. Record an absent optional capability with evidence rather than
  counting it as a passing test. W1-W7 cannot be waived as optional.

For enabled display/active-network edits, separately establish Keep, explicit
Revert and timeout recovery using the installed helper/daemon. Retain the existing
isolated app-loss/partial-write recovery tests as well. A recovery failure blocks
that action even when normal Apply works.

Live acceptance is a separate, deliberate activity from automated fixtures.
Locking, sign-out/reboot, connectivity interruption and waiting for a power
timeout must be coordinated with the user's working session. Do not perform
those actions incidentally while running a test suite.

### RC-5: Collect actionable feedback

- [x] Supply the build identifier, a short known-limitations list, and a simple
  feedback template. Existing background-job details should retain actionable
  errors; a telemetry system or in-app issue tracker is unnecessary.
- [ ] Start a five-working-day usage trial once RC-1 through RC-4 pass. Use Control
  Panel first for the qualified workflows and record occasions that require
  falling back to another tool.
- [ ] Triage configuration/recovery failures first, then blocked tasks, confusing
  interactions and visual polish. Choose the next feature work from observed
  usage, not from the size of the parity inventory.

Suggested feedback record:

```text
Date / build:
What I was trying to do:
Page and steps:
Expected / actual result:
Error text or screenshot, if useful:
Workaround or tool I fell back to:
How much it interrupted the task:
```

The usage trial evaluates the candidate; finishing five days is not a prerequisite
for installing RC1. Critical findings block the next candidate until addressed.

## Execution order and stopping rule

The team should name one owner for each package and one integration owner. These
are suggested responsibility boundaries, not assignments already made:

| Package | Scope and deliverable | Dependencies |
| --- | --- | --- |
| A: Release/install | RC-1 identity, dependency instructions, staged payload, manifest and reversible install procedure | Can start immediately; freeze artifacts after B/C integration |
| B: Save lifecycle | RC-2 migrations, file-target inventory and focused regressions; use Batch 25 as the existing pattern | Can start immediately; one owner coordinates shared registry/controller edits |
| C: Shell/availability | RC-3 behavior, explicit optional-action decisions, layout checks and known limitations | Can start alongside A/B; integrate any changes to session/job behavior with B |
| D: Qualification | RC-4 final build/test evidence, installed scenario results and release verdict | Runs after integrated A/B/C; live actions coordinated with the user |
| E: Usage feedback | RC-5 template, five-day trial and prioritized findings | Materials can be prepared early; normal-use trial follows D |

Avoid concurrent edits to shared CMake, reservation, navigation and controller
interfaces without coordination. Provider work may be split, but the integrated
cross-provider fixtures are required before installation. Do not run simultaneous
live acceptance changes against the user's session.

Implementation order: prepare A/B/C, integrate and freeze one artifact, run its
complete suite and staged install/rollback, install for D, then begin E. Changes
after qualification produce a new build identity and invalidate affected evidence;
run the complete isolated suite on the final artifact and repeat the affected
installed scenarios. Unchanged scenarios may retain evidence with explicit build
traceability. A code change is not covered by an earlier green run by default.

## Handoff evidence and completion

The integration owner publishes one qualification record alongside this document
(for example `docs/mvp-rc1-qualification.md`) with:

- Candidate source/artifact identity, dependency/provider versions and install prefix.
- A link to each work package's changes and relevant checks.
- Final build/full-suite logs, install manifest, staged rollback result and actual
  installed launch result. Copy shareable evidence out of ignored `build/` paths
  or attach it to the handoff; a local-only log path is not sufficient for the team.
- A result for W1-W7 and the modified-provider/recovery checks: **pass**, **fail**,
  **not run**, or **not applicable with evidence** where allowed. Record the
  observer, date/build, before/change/after/restoration and persistence outcomes.
- The exact supported/withheld action list and unresolved issues, distinguishing
  release blockers from follow-up work. Inherited earlier evidence stays labelled.

Mark the RC qualified only when RC-1 through RC-4 and their mandatory scenarios
pass, the RC-5 feedback materials are available, and no blocking defect remains
reachable. The five-day trial follows qualification. Otherwise
report **not qualified** with the remaining work; do not substitute a completion
percentage or the parity-row count for that decision. This acceptance decision
does not mark any whole KDE module complete in the parity backlog.

Stop adding features when these gates pass. APT mutations, additional device and
network families, full shortcut/provider reconciliation, advanced display/power
controls, expanded login/account administration and the rest of native KDE parity
remain follow-up work. Their absence does not prevent this local trial.

The original handoff review changed documentation only. Subsequent A–C work,
the final 107-suite Release run and the exact local installation are recorded in
[the qualification record](mvp-rc1-qualification.md). The candidate is installed
for acceptance and remains **not qualified** until RC-4 and its mandatory live
scenarios pass. Installation and fixture success do not start the five-day trial.
