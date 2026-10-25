# Prompts for the next Codex Cloud agent

Historical handoff: the owner cancelled Cloud migration and chose local
development. Do not execute the Cloud tasks below by default. Follow
[the local development workflow](local-development.md) and current AGENTS.md.

Checkpoint the intended local Batch 25/MVP and manual-update work to GitHub before
handing off a fresh clone. The repository is the durable context. The owner chose
**manual updates in System and Security > Linux Update**, superseding automation.

## Prepare and qualify the Cloud build workflow

```text
Work on SilverGlenn/linux-control, a native Linux Qt/KF6 Control Panel with our
custom Windows 7 Aero UI. Cloud builds tested immutable releases. The laptop user
manually checks, downloads and selects Close and install in Linux Update. Do not
add Actions, timers, startup polling, automatic installation, other themes or a
paid API/CI dependency. A Windows 7 VM is unnecessary.

Read AGENTS.md, docs/manual-updates.md, docs/cloud-development.md,
docs/manual-update-validation.md, docs/local-rc-install.md, docs/mvp-release-candidate.md,
docs/mvp-rc1-qualification.md and docs/mvp-rc-known-limitations.md. Inspect git
status and actual code. Preserve all modified/untracked work including Batch 25.
Confirm the checkout includes manual-update tooling and MVP changes; the old
336bc918 baseline alone does not. Never reset/clean/stash to make setup easier.

You MUST use a real team of subagents. Act as lead and integration owner; spawn
three workers where capacity permits, respecting the environment's hard limit
and never exceeding five concurrent subagents. With fewer slots, use waves.
Allowed subagents ONLY: gpt-6.1-sol at medium/high, or gpt-6-luna at high/xhigh.
Prefer gpt-6.1-sol/high for implementation and independent review. Specify model
and reasoning explicitly; do not inherit an unapproved model or silently substitute.
If the required tools/models are unavailable, report the limitation before
implementation. Show the actual team roster and each worker's scope.

Delegate environment/dependency/build setup, release-driver/packaging work, and
independent safety/testing review. Assign nonoverlapping files; agree shared
interfaces first. You retain shared CMake/core integration and final validation.
Collect the workers' findings, resolve conflicts and run the integrated checks;
delegation is not a substitute for completing the deliverable.

The laptop has RC2 installed, 0.1.0-rc.2+a749642a9c559c3b, with its independent
manual updater already bootstrapped. Its recorded 112/112-suite result and real
no-releases check belong to that exact build. Do not repeat initial RC1 migration
or claim new Cloud/live qualification from those historical results.

Audit the existing ControlPanelUpdateService,
LinuxUpdatePage, MainWindow install guard, InstallationLock, install_lock.py,
control_panel_update.py, rc_bundle.py, bootstrap_manual_updater.py,
make_update_release.py, run_isolated_tests.py and their private fixtures first.

Complete the reproducible Cloud environment and build/release driver. Record
image/compiler/package inputs, required Qt/KF6/KNightTime and approved AeroQt
source/revision/patches or a hashed compatible artifact. Ignored laptop build/deps
and ~/.local libraries are absent from a clone. Do not weaken CMake requirements
or provider guards to hide missing dependencies. Report specific blockers.

Freeze a reviewed exact main commit and compile FROM ITS ARCHIVE for
/home/siru/.local. Run the full discovered suite through run_isolated_tests.py.
Retain logs and source/build/payload/runtime identities. Stage exact final bytes;
exercise actual-payload install/upgrade/rollback/remove/interruption recovery in
private roots. Make the required matching validation record, then use
make_update_release.py. Packaging alone does not run or certify those checks.

Preserve Aero-only UI, navigation/job lifetime, Apply/Revert, provider/root trust
guards and display/active-network withholding. Do not change real charging,
audio, display, network, power or KDE settings; lock/sign out/reboot; or install
anything on the laptop. No laptop SSH, credentials, buses or raw KDE files are
needed. The existing deployment journal must remain intact. Cloud dependency
provisioning belongs in the sandbox, never on the owner's laptop. Review
trust/recovery paths and run meaningful fixtures.

Historical 107-suite RC1 and 112-suite RC2 results are not new-build evidence. Separate cloud
fixtures, staged payload checks, real attestation verification and installed
hardware observations. Do not complete RC-4/W1-W7 or start the five-day trial from
fixtures. Finish with reviewable changes, exact commands/results, dependency
provenance, remaining live gates and a tested publication procedure. Do not claim
the Cloud pipeline works until it has built and verified a real artifact.
```

## Publish after the build workflow is qualified

Use a separate trusted release environment with repository publishing authority;
arbitrary PR coding environments do not need those credentials.

```text
Publish the next manual laptop-candidate update for SilverGlenn/linux-control
from a reviewed exact main commit. Read AGENTS.md and current manual/cloud/RC docs.
No Actions, timers, polling or automatic laptop installation.

Run the qualified Cloud release driver: freeze source, build its archive for the
target prefix, run the complete isolated suite, stage final bytes, exercise exact
payload deployment/recovery and record dependency/runtime evidence. Stop on
mandatory failures. Do not filter tests, weaken guards, rebuild after validation
or substitute historical results.

Package the exact bundle and matching evidence with make_update_release.py.
Allocate a new sequence. Create a draft release/tag
laptop-candidate-<sequence>-<commit12>, setting target_commitish to the full SHA.
Attach release.json, control-panel-bundle.tar.gz, validation.json and retained
evidence. Compare draft asset bytes against the descriptor before publishing.

Publish with immutability. Verify the release and BOTH consumed assets using
gh release verify / gh release verify-asset against the explicit repository.
Without credentials or valid attestations, retain artifacts and report the
blocker; never use checksum-only fallback. The laptop independently verifies
when the user requests an update.

Label the release candidate for acceptance; installed for acceptance only after
confirmed laptop installation; qualified only after its exact live gates pass.
Preserve pending W1-W7 and reopen affected checks. Do not update/restart the
laptop or run live desktop tests from Cloud. Return release URL, full commit,
build/source/payload identities, results, limitations and remaining acceptance.
```
