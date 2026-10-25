# Local acceptance and release tooling

25 October 2026. Integration owner: root. The preserved RC2/MVP/Batch 25 work was
checkpointed and pushed to fork main as `6148cdc`. The checkpoint comparison and
labelled publication redaction are recorded in
[the evidence summary](evidence/checkpoint-2026-10-25.json).

The installed build is now **`0.1.0-rc.4+4ca694bb908b1211`**, not qualified.
RC2's [112-suite record](manual-update-validation.md) remains historical for that
exact artifact. No fixture result below completes physical setting acceptance.

RC3 `0.1.0-rc.3+a6a4987093bf32d3` was built from reviewed main commit
`70966b4e49975c018144564bab93cf899d69be0e` and published for acceptance as
[an immutable candidate release](https://github.com/SilverGlenn/linux-control/releases/tag/laptop-candidate-1-70966b4e4997).
The installed RC2 application's Check and Download actions verified that exact
candidate, then Close and install installed it successfully. All 12 installed
files and modes matched; the reopened GUI title and provider-free version matched
RC3. Integration owner: root. No physical workflow is qualified by publishing
or installing a release.

The follow-up RC4 `0.1.0-rc.4+4ca694bb908b1211` is now built from reviewed main
commit `ce2a444d132f63b92f01a6f7836a786ac0612260` and
[published immutably](https://github.com/SilverGlenn/linux-control/releases/tag/laptop-candidate-2-ce2a444d132f).
Its complete **114/114 suites pass, 128.38 seconds**, with 12 managed files and
130 runtime fingerprints. All release/asset attestations passed the pinned
verifier. A second staging run using the exact RC3 bundle as predecessor passed
real-byte upgrade, rollback, removal, conflict refusal and interruption/recovery
after `bin/control`. This remains private-prefix fixture evidence, not an
installed rollback or physical qualification. See
[the exact RC4 build record](evidence/local-rc4-build.json).

RC4 fixes the Sound accessible-selection/editor mismatch and exposes full Sound
details/update label values as accessible descriptions. It preserves lifetime,
read-only, refresh and compatibility guards. After the owner closed all windows,
the installed manual workflow delivered RC4. All 12 installed files/modes and
GUI/headless identities matched. The actual local-result button exposed the
full installed status without an online check. An independent trusted-installer
cycle restored exact RC3 files/version and then the verified RC4 local bundle,
retaining the original journal. No receipt replay, quarantine reset or updater
state edit was used. This is distinct from live consumer failure/quarantine
handling, which was not exercised. See [real delivery](evidence/local-rc4-delivery.json).

## Current installed observations

| Scenario | Observation |
| --- | --- |
| W1 terminal/GUI identity on RC4 | Absolute installed executable, provider-free version and accessibility frame identify exact RC4; all payload files/modes match |
| W1 assistive search on RC4 | All six required destinations return matching result items: Sound, Mouse, Desktop Background, Night Light, Power Options and Personalization |
| W1 visible menu, keyboard and pending work | Not yet fully observed on RC4; W1 remains open. Historical GIO desktop-entry launch is not a visible-menu click |
| W2 Bluetooth on RC4 | Owner confirmed sound/silence/sound; UI volume 20→15→20 and mute off→on→off independently read back; exact baseline restored |
| W2 speakers/default on RC4 | Owner confirmed laptop speakers at 50%; actual new test stream routed to analog-output-speaker. Sound reopened while changed and showed 50%/unmuted/default speaker. Restored exact speaker 0%/muted and Bluetooth default; preserved freshly captured Bluetooth 31% baseline |
| W2 persistence | Later sign-in/reboot with changed persistent values has not been coordinated. Reopen and restoration do not prove that persistence; W2 remains open |
| W3-W7 and modified providers | Not run; coordinate exact changes, physical observation, restoration and persistence with the owner |
| Charging policy/config baseline | All 13 monitored paths match after the completed checks: three charging policies and ten configuration paths, including absent kfontinst.rc. Earlier drift classification was an absence-label comparison error; see the explicit correction below |
| Update status accessibility on RC4 | Actual AT-SPI descriptions expose full current build and installed local result; focused fixtures cover changing status/metadata and native notifications |

RC2 assistive search failed, and RC3 exposed the selected-row/current-editor
mismatch; both were reproduced with real Qt interfaces and corrected before
RC4. The RC3 speaker probe transiently changed Bluetooth instead and restored
it; those observations remain in [the historical record](evidence/local-rc3-audio.json).
The RC4 speaker test at 15% was too quiet for physical confirmation. After phone
audio interrupted an attempt, the owner requested repeats and a higher level;
50% was audible. Every phase restored its captured baseline. See
[the exact RC4 audio observations](evidence/local-rc4-audio.json).

[Baseline comparison correction](evidence/local-baseline-correction.json): the
baseline represented the missing `kfontinst.rc` as `absent: true`, while later
comparison code used `missing: true`. That label mismatch falsely reported drift.
The file was absent at baseline and later checkpoints and remains absent. The
corrected comparison matches all 13 paths. Earlier raw reports remain private;
prior Git/source snapshots retain the erroneous report, explicitly superseded
here rather than treated as evidence of a configuration mutation.

The corrected source now has **15 passing Sound Qt cases** and **17 passing
updater Qt cases**, with no failed/skipped cases in those focused runs. Real Qt
accessibility Selection regressions first failed across all three Sound tabs;
the fix aligns selection/current editor and disables a cleared selection, while
preserving refresh and lifetime checks. Sound details and update labels expose
their full text as accessible descriptions. These focused results remain private
fixture evidence; the separate RC4 full-build and live records above identify
what was actually checked on the installed artifact.
See [the focused correction record](evidence/local-rc4-focused.json).

Raw accessibility trees, device identifiers and baseline records stay private
under `build/local-acceptance/w1-private`. The initial W1 window is agent-owned;
no existing user window or pending user settings were modified. Desktop-entry
activation must be distinguished from an actual visible-menu observation.

## New tooling validation

`build_local_release.py` and `validate_release_payload.py` have an independent
review and 29 passing private fixtures, including a miniature frozen-source
CMake build, complete CTest execution, actual staged transactions and packaging.
The two CTest registrations also pass. The packager's existing 12 fixtures pass
in the same isolated discovery run. Required compiler/readelf absence fails the
staging suite; it cannot silently skip its regressions.

The search fix's focused isolated suite passes 12 Qt test rows, with zero failures;
one pre-existing optional screenshot case was skipped in that focused run. That
is not evidence of completed installed keyboard or desktop acceptance.

The driver requires clean actual main for a release. Dirty/non-main rehearsal is
explicit and cannot emit updater release validation or packaging. Development
pins reject escaping links; complete discovered CTest names/status must match the
executed JUnit record; runtime inventory and exact bundle hashes are rechecked.
Staging uses private prefixes and a clearly labelled synthetic metadata predecessor
when no previous bundle is supplied. It tests one interruption boundary, not
every power-loss boundary. No tooling command installs on the laptop or publishes.

## Complete RC3 build and release

The first complete real project run passed: **114/114 isolated CTest suites**,
zero failed/not run, 126.01 seconds. Source was frozen from clean actual main,
then extracted and built as Release for `/home/siru/.local`. Staging verified
12 managed files and 130 resolved runtime dependencies. Install, upgrade,
rollback, removal, conflict refusal, headless runtime and one interruption/recovery
boundary passed. The predecessor was a **synthetic metadata fixture**: executable
bytes unchanged, desktop comment and fixture marker changed. This is not a real
prior-release rollback result or exhaustive crash testing.

The draft's full commit, exact asset set, sizes and GitHub SHA-256 digests matched
local files before publication. Repository immutable releases were enabled, then
the laptop's independently pinned verifier passed `release verify` and
`verify-asset` for `release.json`, `control-panel-bundle.tar.gz` and
`validation.json`. No GitHub Actions or remote builds were used. Attestation
verifies provenance; the locally retained build/test evidence supports test claims.

See [exact build identities and evidence hashes](evidence/local-rc3-build.json)
and [installed RC2 observations](evidence/local-installed-observations.json),
[real RC3 delivery](evidence/local-rc3-delivery.json) and
[partial coordinated audio observations](evidence/local-rc3-audio.json).
Complete private logs are under
`build/local-acceptance/release-checkout/build/local-rc3` and
`build/local-acceptance/release-verification`.

These results belong to RC3. The subsequent RC4 build, installed delivery,
independent app-file rollback/restoration and Sound observations are recorded
above with their own identities. Live qualification still requires remaining W1,
W2 changed-value persistence, W3-W7 and the modified-provider scenarios. No
sign-out/reboot was performed. The five-day trial has not started. Preserve the
existing journal and compatibility guards; do not expand categories or silently
waive a mandatory workflow.
