# Local acceptance and release tooling

25 October 2026. Integration owner: root. The preserved RC2/MVP/Batch 25 work was
checkpointed and pushed to fork main as `6148cdc`. The checkpoint comparison and
labelled publication redaction are recorded in
[the evidence summary](evidence/checkpoint-2026-10-25.json).

The installed build remains `0.1.0-rc.2+a749642a9c559c3b`, not qualified.
Its [112-suite record](manual-update-validation.md) is historical for that exact
artifact. No fixture result below completes physical setting acceptance.

## Current installed observations

| Scenario | Observation |
| --- | --- |
| W1 terminal launch | Separate agent-owned installed window launched by absolute executable; accessibility frame reports the exact RC2 build |
| W1 assistive search | **Failed on RC2:** AT-SPI editable-text operations change the search field, but search results stay on the old page |
| W1 proposed fix | Actual Qt accessibility regression reproduced two failures; guarded textChanged debounce fixes it; search/history/Cancel/read-only fixture regressions pass. New artifact build/install/recheck pending |
| W1 menu, keyboard, remaining destinations and pending work | Not yet fully observed on the installed artifact; W1 remains open |
| W2 prerequisite refresh | Two real sinks now observed (ALSA/PCI and BlueZ/Bluetooth), both suspended; the older single-output inventory is no longer the current blocker. No route/volume/mute setters used; audible and default-output acceptance remains open |
| W3-W7 and modified providers | Not run; coordinate exact changes, physical observation, restoration and persistence with the owner |
| Charging policy/config baseline | All three charging-policy fingerprints and ten monitored configuration-file fingerprints unchanged through the initial navigation-only observations |

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

First complete project build, final full suite and staged runtime checks remain
pending. Live qualification, first immutable-release delivery, and the five-day
trial remain open. Keep release/hardware evidence separate and preserve the
existing deployment journal and compatibility guards.
