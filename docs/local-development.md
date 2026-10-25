# Local development and the next release milestone

The owner cancelled the Cloud migration. Develop, build and investigate integration
on this KDE laptop using its installed Linux providers and custom Aero assets.
The manual updater remains in System and Security > Linux Update. GitHub can hold
source and release downloads; no remote build service is required.

## Current position

RC4 `0.1.0-rc.4+4ca694bb908b1211` is installed for acceptance with the separate
trusted updater. Its complete 114/114 isolated suites and staged checks passed;
the real published immutable-release Check/Download/Close and install/reopen/local
result path and independent app-file rollback/restoration also passed. Sound
effects, changed-state reopen and baseline restoration have owner observations;
persistence remains open. See [the current evidence](local-acceptance.md). These
results do not complete desktop/hardware qualification or live consumer failure/
quarantine handling.
RC2's [112-suite record](manual-update-validation.md) remains historical.

The preserved Batch 25, MVP and updater work was checkpointed to fork main as
`6148cdc`; the reviewed local release tooling and accessibility search fix are
`70966b4`. RC4 from `ce2a444` fixes the subsequent accessible Sound selection and
status-description findings and passed its complete 114-suite run. It is
published and installed for acceptance; remaining physical qualification stays
separate. See [the current record](local-acceptance.md).
Exclude ignored build products, raw KDE baselines, backup bytes and credentials
from future checkpoints. Preserve existing changes; do not reset, clean or stash
to manufacture a clean release checkout.

## Next batch: qualify the installed MVP

Keep the bounded daily-use goal; avoid adding settings categories during this
batch. Follow [RC-4 and its mandatory workflows](mvp-release-candidate.md).

1. Start with W1: confirm menu/terminal build identity, search destinations,
   Back/Home, keyboard navigation and retention of pending work.
2. Coordinate W2-W5 and W7 individually: Sound, pointer/scroll preference,
   wallpaper, Night Light and supported Aero tuning. Capture baseline, apply,
   observe the real effect, reopen and restore. Record failures honestly.
3. Refresh the audio inventory before W2. The latest passive check found two real
   outputs (built-in and Bluetooth), superseding the historical single-output
   blocker. Do not waive the required default-output change or replace it with a
   virtual sink fixture. Actual effects and restoration still need observation.
4. Run power acceptance last, coordinating the timeout and preserving the three
   charging-policy fingerprints before/after. Do not use a charging setter as
   part of qualification or infer the old incident's actor.
5. Complete the additional modified-provider scenarios required by RC-4: font
   role, keyboard repeat, title-bar action, default terminal and common shortcut.
   Coordinate any consumer restart and the later sign-in/persistence observation.
6. Fix findings, then freeze/build a new artifact, run its full suite and staged
   recovery checks, and repeat affected acceptance. Old fixture passes cannot
   qualify changed code. Keep display/active-network mutation withholding.

Physical behavior needs an observer; running locally does not make readback a
substitute for visible effects or persistence. No disruptive live settings,
timeout, connectivity, lock, sign-out or reboot tests happen without coordinating
with the owner. Parallelize implementation/audits, not live desktop mutations.

Start the five-working-day feedback trial only after the mandatory release gates
pass. Triage observed configuration loss, blocked tasks and confusing interactions
before expanding KDE parity. Keep one identified build installed during the trial.

## Local release and update delivery

`tools/build_local_release.py` provides the repeatable local command. It freezes
source, builds its archive for `/home/siru/.local`, runs all discovered fixtures
with `tools/run_isolated_tests.py`, stages the payload and independently validates
rollback/recovery with `tools/validate_release_payload.py`. Miniature real-build
fixtures and the complete RC3 project run pass, as tracked in
[the local acceptance record](local-acceptance.md).
Record actual Qt/KF6/KNightTime/AeroQt and external development/runtime inputs.
Existing ignored development caches are inputs to document, not deployable app
dependencies. Never lower compatibility or root trust guards to make a build fit.

Use `tools/rc_bundle.py` and the existing canonical journal for reviewed local
installations. Replacing the app requires it to be closed and all jobs finished;
keep the independent repair tool and original backup history intact. AeroQt,
themes, KDE preferences and system packages remain outside payload ownership.

When a release is ready for the manual GitHub download path, checkpoint a reviewed
main commit, create matching validation evidence and use
`tools/make_update_release.py`. Publish/verify immutable assets under
[the existing contract](manual-updates.md). Prove one real download/apply/reopen/
rollback cycle separately; fixture packaging is not that observation. Do not
change the updater to execute arbitrary local or downloaded installers to bypass
verification. There are no timers, startup polling or automatic installations.

The formal MVP usage trial does not require a public release or Cloud pipeline.
Local builds and existing reversible local installation can support qualification
while the published-release end-to-end check remains a separate updater gate.

Release builds require a clean `main` checkout. A private configuration pins
development trees/files and external runtime roots; inspect digests with
`python3 tools/build_local_release.py --input-digest /absolute/input`, then verify
the completed configuration with `--check-config /absolute/config.json`.
The configuration has schema 1, `cmake_definitions` (dependency-location keys
only), `development_inputs` and `runtime_dependencies` (`path`/`sha256` pairs).
Record original development archives as well as extracted trees. Full linked
runtime dependencies are discovered and hashed separately.

```bash
python3 tools/build_local_release.py \
  --root "$PWD" --output "$PWD/build/NEW-RELEASE" \
  --config "$PWD/build/local-acceptance/release-inputs.json" \
  --version 0.1.0-rc.3 --jobs 2
```

The output parent must exist and the output directory must be new; in-checkout
output must be ignored. The driver logs every phase and only emits
`validation.json` after actual complete-suite and staged checks pass. It never
installs live or publishes. Optional `--sequence N` packages through the existing
release contract; it does not publish. Explicit `--rehearsal` accepts dirty or
non-main development snapshots, forbids updater packaging, and writes separately
labelled `rehearsal-validation.json`. Rehearsal evidence is not release evidence.

The recorded laptop runs use a separate user systemd scope to avoid inheriting
the terminal's smaller memory cap: prefix the command with
`systemd-run --user --scope --quiet -p MemoryHigh=2G -p MemoryMax=3G`, keeping
`--jobs 2`. This scopes build resources; it installs no service or update timer.
Once a previous exact bundle exists, also run `validate_release_payload.py`
with `--previous-bundle` to validate actual prior-release bytes in staging.
The driver's default synthetic metadata predecessor is labelled separately.

## Team coordination

For implementation batches, keep one integration owner and divide independent
work among permitted subagents: GPT-6.1 Sol medium/high or GPT-6 Luna high/xhigh.
Prefer three workers for implementation, fixtures and independent review when
useful; never exceed five concurrent subagents or the environment's hard limit.
Assign shared CMake/controller/installer files and agree interfaces before edits.
Collect their evidence and run the integrated checks. Live qualification stays
sequential and coordinated with the owner.
