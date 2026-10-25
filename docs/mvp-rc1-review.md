# Local RC1 repository review

Reviewed on 25 October 2026 before implementation. Integration owner: **root**.
The local daily-use candidate is the current target; full KDE parity remains a
later milestone. The plan is sound with the clarifications below. None of these
findings qualifies a live workflow or authorizes incidental desktop disruption.

## Preserved starting point

HEAD is `336bc91882cdab8a1625e95de173160a607974d0`. Nineteen modified/untracked
files, their byte hashes and the existing binary diff were preserved privately
under `build/mvp-rc-review-20261025-start` before editing. This includes Batch 25,
the new MVP plan and the user's README/replacement-plan additions. No reset,
stash, checkout or cleanup was used. The candidate must identify the integrated
working tree, rather than HEAD alone.

Batch 25's recorded 101-suite pass and 410-input manifest remain **inherited
fixture evidence**. They do not cover subsequent RC implementation changes and
do not prove live effects, persistence, installation or charging-incident
attribution. New checks remain pending until explicitly recorded.

## Findings and concrete corrections

| Finding | Evidence at review | Correction / gate |
| --- | --- | --- |
| **W2 qualification blocker:** only one available playback sink, already default; headphone/HDMI alternatives unavailable | Passive PipeWire 1.6.2 inventory, privately recorded in `audio-prerequisite.json`; W2 in the release plan | Connect a second supported real output for the default-output change. Keep W2 mandatory and pending; neither reselecting the same sink nor a virtual null sink passes physical playback. User coordination requested. No audio setter ran. |
| Fresh-build dependencies are incompletely described | `CMakeLists.txt` dependency declarations; existing cache uses extracted Positioning, KNightTime, Holidays and CUPS development files | Record matching development package or extracted-prefix provenance and hashes separately from runtime installed libraries. Preserve provider/version guards and configure the final absolute prefix before compilation. |
| Current copy-one-binary guidance cannot install the present payload | README binary-install section; `CMakeLists.txt` install rules | Supply a payload manifest and reversible multi-file procedure. Retain originals and refuse intervening edits. AeroQt/theme dependencies stay outside application ownership. |
| Menu launch is PATH-sensitive; identity is absent | `linux-controlpanel.desktop` uses `Exec=control`; `src/main.cpp` immediately constructs QApplication; shell title has no build ID | Generate an absolute installed launcher, add a pre-GUI version/help path and visible build identity, and hash included modified/untracked source plus distributed binaries. |
| RC-2 is six provider lifecycles, rather than four sync call moves | `DefaultAppsBackend.cpp` and `StandardShortcutsBackend.cpp` own running watcher/queues in the backend; Font discovery and all four active native readers run synchronously | Move reads/discovery/transactions/cleanup/readback to worker values. Keep app-owned mutation/service/activity/reservation continuations for all six providers. Retain intermediate identities and partial-save/owned rollback semantics. |
| Provider replacement can occur during a worker's lock wait | Aero/Appearance runtime interfaces and well-known-name KWin reloads | Latch invalidation, preserve existing version checks, pin owner/epoch where available and check before unsent reload. Document native Notify and sent-call uncertainty; do not promise atomicity or remote cancellation. |
| Display and active-network recovery is not yet qualified live | `KdeDisplayBackend::beginPreview`; `NetworkManagerBackend::beginWrite` checkpoint paths | Withhold production display mutations and connectivity/active-profile mutations at backend and UI boundaries. Keep discovery and inactive saved-profile edits. Preserve private recovery tests; optional actions receive no live credit. |
| Privileged user-local helpers cannot become trusted through copying | `TimeSyncConfig.cpp` root trust checks; `AeroLoginBackend.cpp` helper/policy/ancestor identity checks | Keep time-server/login editing unavailable, with explicit user-local limitations. Preserve other supported Date/Time operations; no system-wide helper/policy installation. |
| APT documentation conflates missing mutation support with existing preview | README package-management limitations versus existing APT preview | Say APT mutations remain unsupported; retain read-only planning. Do not add APT mutation work. |
| Layout and physical adoption remain untested | Existing fixed-width forms and offscreen fixture captures | Check actual scale/geometry and keyboard use; fix observed unusable layout only. Keep installed effects, sign-in and restoration in coordinated RC-4. |

Source references describe the preserved review snapshot; implementation may move
lines. The source archive/manifest will make the final candidate independently
identifiable. W1–W7 and the representative modified-provider checks remain
mandatory. A missing capability cannot silently become a pass.

## Assignments and shared-file coordination

| Package | Owner | Work / dependency |
| --- | --- | --- |
| A: Release/install | `rc_release` | Identity API/CLI, source/payload provenance, conservative install/upgrade/remove tooling and private regressions; freeze artifact after B/C integration |
| B: Save lifecycle | `rc_lifecycle`, coordinated with root | Aero Theme and Keyboard owned by `rc_lifecycle`; Appearance owned by root; Font assigned to `rc_release` after A preparation; Default Apps/Standard Shortcuts assigned to `rc_shell` after its initial C work. Six-provider inventory and shared acceptance remain B's deliverable |
| C: Shell/availability | `rc_shell` | Optional-action backend/UI gates, shell regression gaps, helper/Ubuntu limitations, visible identity integration and layout review |
| D: Qualification | root | Final source/build identity, single Release builder, full isolated suite, staging/runtime/rollback gates, exact installed artifact and coordinated live scenarios |
| E: Feedback | root | Known limitations/feedback materials and qualification record; five-day trial only after RC-1–RC-4 pass |

Root owns `CMakeLists.txt`, `SettingsFileReservations.*`, `OperationManager.*`,
shared cross-provider fixtures, and the main plan/review/qualification documents.
Agents propose shared-interface changes before editing them. C owns the small
MainWindow identity change and coordinates A's read-only identity API. Backend
and controller pairs have one owner. Existing Batch 25 edits are preserved in
shared tests and CMake.

No concurrent builders or live acceptance changes. All implementation regression
tests use private files, fake providers and private buses. Live mutations,
locking/sign-out/reboot, connectivity changes and power timeout waits require
coordination with the user. Installation waits for integrated RC-1–RC-3 and final
isolated/staged evidence. The RC is **not qualified** at this review.

## Implementation outcome

A–C preparation is complete and the candidate is installed for acceptance. The
[qualification record](mvp-rc1-qualification.md) separates the final 107-suite
Release pass, staged deployment/loader evidence and headless installed identity
from the unrun live workflows. Existing Batch 25 source/tests and its audit
records remain preserved. Six provider lifecycles were completed without adding
settings categories or migrating the retired legacy writers.

Integration review repaired exact native-inode ownership gaps around Standard
Shortcuts rollback and Font's XML/KDE transition, a missing final Aero alias
check, and native Font readback normalization. The latter follows KConfig 6.24's
deliberate handling of four normal-weight face aliases, preserving other names
and weights. Focused regressions and the complete suite pass; these are fixture
results. New shell-fixture accessor/type errors and layout-settling assertions
were corrected without dropping cases or enlarging windows.

W2's single-output prerequisite and coordinated installed/live/sign-in checks
remain qualification blockers. No mandatory workflow was waived. Result documents
and shareable evidence are published after the immutable source snapshot;
compiled inputs and the installed payload retain that snapshot's identity.
