# Manual update RC2 validation

Status: **installed for acceptance; not qualified**. Integration owner: root.
Date: 25 October 2026. The owner coordinated closing RC1 before migration.

| Item | Result |
| --- | --- |
| Installed build | `0.1.0-rc.2+a749642a9c559c3b` |
| Baseline | `336bc91882cdab8a1625e95de173160a607974d0`, plus preserved Batch 25/MVP and manual-update work |
| Frozen inputs | 505; built from extracted archive with source bytes/modes checked |
| Source manifest SHA-256 | `a749642a9c559c3bde1c69add8271f88c424f5278438be786fc1c7a739bf5738` |
| Source archive SHA-256 | `95b9d778fa910c9118248ca676202dce4e84d6eb40f9f05965eed17e73040050` |
| Payload manifest SHA-256 | `0b1cfc97330cd44614cbf900c88d8feace7f5eb1328f95d02125b2d6ac7602e2` |
| Complete isolated suite | **112/112 pass, 292.92 seconds**; private HOME/XDG, activation-free buses, offscreen/fake providers |
| Focused development run | 12/12 suites passed before final artifact build; final full suite above is authoritative |
| New Python fixtures | Consumer 38, packager 12, bootstrap 5; installer now 15 |
| Runtime inventory | 130 dependency hashes; staged executable loads without checkout/development paths, external AeroQt supplied separately |
| Actual staged payload | Install/upgrade/rollback/remove, original bytes/modes/symlink, unrelated dependencies, conflict refusal and one deliberate interruption/recovery pass |
| Installed application | All 12 managed files/modes match the exact bundle; headless identity matches |
| Rollback journal | Existing `/home/siru/.control-panel-rc-deployment` preserved; old app/history retained |
| Trusted updater | Independent private runtime/config bootstrapped; reviewed installed GitHub CLI pinned; verification commands available |
| Installed local status | Works without network; initial idle/no candidate prepared |
| Installed explicit online check | **No published candidate releases**, real read-only request, matching the independently checked GitHub release listing |
| Aero UI | Final fake-provider capture reviewed; readable text/actions and installation/close explanation; not a live GUI observation |
| Charging policy | Three passive policy fingerprints unchanged after fixtures, install, bootstrap and manual check |

See [shareable logs, manifests and capture](evidence/manual-updates-rc2/README.md).
The immutable source archive and complete private execution artifacts remain at
`build/manual-updater-work/bundle/source/source.tar.gz` and
`build/manual-updater-work/`. Result documents/evidence are updated after the
freeze; code, tests, build instructions and tool inputs remain identical to the
archived snapshot. Historical parity/backlog and RC1 evidence are not rewritten.

The implementation is manual: System and Security > Linux Update > Check for
Control Panel updates > Download update > Close and install. No timer/service,
startup polling, automatic installation, forced close or desktop settings writes
were introduced. No app window was opened by the agent during installation.

Workflow decision after installation: the owner cancelled Cloud migration.
Development/builds continue on this laptop; no Cloud environment is a release
prerequisite. This changes the plan, not the recorded artifact results above.

Remaining gates: repeatable local release builds; the first actual
immutable release and both asset attestations; installed check/download/apply of a
real release, failure/rollback acceptance; installed GUI/menu/sign-in checks; all
RC-4/W1-W7 and modified-provider hardware observations. W2 still needs a second
real output. The five-day trial has not started. Fixture and headless passes do
not qualify those live effects. Coordinate disruptive checks with the owner.

Independent recovery/rollback and the Cloud handoff are documented in
[the manual update workflow](manual-updates.md) and
[local development plan](local-development.md). Checkpoint the preserved local
work to GitHub; the remote baseline alone lacks it.
