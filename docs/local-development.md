# Local development and the next release milestone

The owner cancelled the Cloud migration. Develop, build and investigate integration
on this KDE laptop using its installed Linux providers and custom Aero assets.
The manual updater remains in System and Security > Linux Update. GitHub can hold
source and release downloads; no remote build service is required.

## Current position

RC2 `0.1.0-rc.2+a749642a9c559c3b` is installed for acceptance with the separate
trusted updater. Its recorded 112/112 isolated suites, staged deployment/recovery
and headless identity checks passed. See [the exact evidence](manual-update-validation.md).
Those results belong to that artifact. They do not complete real desktop/hardware
acceptance or prove a published-release download/install. The recorded manual
GitHub check found no candidate releases.

Existing Batch 25, MVP and updater work is still present locally. Review and
checkpoint the intended source/tests/tools and sanitized evidence to GitHub.
Exclude ignored build products, raw KDE baselines, backup bytes and credentials.
Preserve existing changes; do not reset, clean or stash to manufacture a clean
release checkout.

## Next batch: qualify the installed MVP

Keep the bounded daily-use goal; avoid adding settings categories during this
batch. Follow [RC-4 and its mandatory workflows](mvp-release-candidate.md).

1. Start with W1: confirm menu/terminal build identity, search destinations,
   Back/Home, keyboard navigation and retention of pending work.
2. Coordinate W2-W5 and W7 individually: Sound, pointer/scroll preference,
   wallpaper, Night Light and supported Aero tuning. Capture baseline, apply,
   observe the real effect, reopen and restore. Record failures honestly.
3. Refresh the audio inventory before W2. The last recorded blocker was only one
   physical playback output; do not waive the required default-output change or
   replace it with a virtual sink fixture. Other acceptance can proceed meanwhile.
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

The next tooling follow-up is a repeatable local command that freezes source,
builds its archive for `/home/siru/.local`, runs all discovered fixtures with
`tools/run_isolated_tests.py`, stages the payload and validates rollback/recovery.
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

## Team coordination

For implementation batches, keep one integration owner and divide independent
work among permitted subagents: GPT-6.1 Sol medium/high or GPT-6 Luna high/xhigh.
Prefer three workers for implementation, fixtures and independent review when
useful; never exceed five concurrent subagents or the environment's hard limit.
Assign shared CMake/controller/installer files and agree interfaces before edits.
Collect their evidence and run the integrated checks. Live qualification stays
sequential and coordinated with the owner.
