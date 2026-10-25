# Local acceptance and release tooling

25 October 2026. Integration owner: root. The preserved RC2/MVP/Batch 25 work was
checkpointed and pushed to fork main as `6148cdc`. The checkpoint comparison and
labelled publication redaction are recorded in
[the evidence summary](evidence/checkpoint-2026-10-25.json).

The installed build is now `0.1.0-rc.3+a6a4987093bf32d3`, not qualified.
RC2's [112-suite record](manual-update-validation.md) remains historical for that
exact artifact. No fixture result below completes physical setting acceptance.

RC3 `0.1.0-rc.3+a6a4987093bf32d3` is built from reviewed main commit
`70966b4e49975c018144564bab93cf899d69be0e` and published for acceptance as
[an immutable candidate release](https://github.com/SilverGlenn/linux-control/releases/tag/laptop-candidate-1-70966b4e4997).
The installed RC2 application's Check and Download actions verified that exact
candidate, then Close and install installed it successfully. All 12 installed
files and modes match; the reopened GUI title and provider-free version match
RC3. Integration owner: root. No physical workflow is qualified by publishing
or installing a release.

## Current installed observations

| Scenario | Observation |
| --- | --- |
| W1 terminal launch | Separate agent-owned installed window launched by absolute executable; accessibility frame reports the exact RC2 build |
| W1 assistive search | **Failed on RC2:** AT-SPI editable-text operations change the search field, but search results stay on the old page |
| W1 search fix | Actual Qt accessibility regression reproduced two failures; guarded textChanged debounce fixes it; search/history/Cancel/read-only fixture regressions pass. Installed RC3 assistive Sound query produced actual result list items; the first probe incorrectly expected link roles. Remaining destinations/keyboard/drafts stay open |
| W1 menu, keyboard, remaining destinations and pending work | Not yet fully observed on the installed artifact; W1 remains open |
| W2 Bluetooth phase on RC3 | Owner coordinated the test and confirmed sound/silence/sound; UI volume 47→42→47 and mute off→on→off independently read back; exact baseline restored. This is partial W2, not full Sound acceptance |
| W2 speaker phase | Probe found visual accessible selection could differ from the current-row editor; Bluetooth was transiently changed instead of speakers and restored to its refreshed 20% baseline. Speakers stayed 0%/muted, default stayed Bluetooth. Source fix and isolated regression pass; new artifact/default-output acceptance remain open |
| W3-W7 and modified providers | Not run; coordinate exact changes, physical observation, restoration and persistence with the owner |
| Charging policy/config baseline | All three charging-policy fingerprints unchanged after real update and audio probes. Initial navigation preserved all ten config fingerprints; later kfontinst.rc drift was observed, unattributed and not overwritten; other nine config files matched |
| Update status accessibility | On installed RC2/RC3 the three labels' fixed accessible names hide actual build/status/metadata text from AT-SPI; sighted UI status and private helper result are separate evidence. Source repair and focused regression coverage pass; new artifact/live recheck pending |

The corrected source now has **15 passing Sound Qt cases** and **17 passing
updater Qt cases**, with no failed/skipped cases in those focused runs. Real Qt
accessibility Selection regressions first failed across all three Sound tabs;
the fix aligns selection/current editor and disables a cleared selection, while
preserving refresh and lifetime checks. Sound details and update labels expose
their full text as accessible descriptions. These are private fixture results;
a new frozen artifact/full suite and installed rechecks are still required.
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

Real published-release check/download/install/reopen passed. The local-result
button and actual installed rollback cycle remain open, as do live qualification
and the five-day trial. Keep release/hardware evidence separate and preserve the
existing deployment journal and compatibility guards. The corrected source will
need a new exact artifact/full checks before affected live rechecks; RC3 evidence
cannot qualify those changes.
