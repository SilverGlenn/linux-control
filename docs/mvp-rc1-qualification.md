# Local RC1 qualification record

Historical record: RC1 was replaced locally on 25 October 2026 by the
[manual-update RC2 candidate](manual-update-validation.md). The observations and
pending acceptance below remain specific to RC1; they are not new-build results.

Status: **installed for acceptance; not qualified**. RC-1–RC-3 preparation gates passed; live RC-4 qualification and the five-day trial remain open.
Integration/qualification owner: **root**. Review date: 25 October 2026.

The [release plan](mvp-release-candidate.md) defines mandatory gates and the
[repository review](mvp-rc1-review.md) records corrections and ownership. The
[known limitations](mvp-rc-known-limitations.md) describe intended availability;
they do not assert observed live acceptance.

## Candidate identity and checks

| Item | Result |
| --- | --- |
| Source baseline | `336bc91882cdab8a1625e95de173160a607974d0`, plus preserved Batch 25 and MVP work; 463 archived inputs |
| Candidate | `0.1.0-rc.1+7584b7fda4e74a21` |
| Source manifest SHA-256 | `7584b7fda4e74a21bf5cdf7102b3c86001e1caa8209c5cedf7e72c872ba8a34b` |
| Source archive SHA-256 | `5aea33c14f38b58e4cfcc2c3e5495403f40a24697ee1bac46d1d379a28c4de47` |
| Prefix / build | Fresh Release directory `build/rc1-release`, configured for `/home/siru/.local` before compilation |
| Dependencies | Installed package identities; eight cached development-package hashes; 188 extracted development-file hashes; 130 installed ELF dependency hashes |
| Complete isolated suite | **107/107 pass, 211.67 seconds** on the final snapshot; earlier Batch 25 101-suite evidence remains historical |
| Focused regressions | **30/30 pass, 60.78 seconds**, including all 14 cross-provider ownership cases |
| Installer fixtures | Nine Python fixtures pass, also retained in the full CTest run; actual staged payload install/upgrade/rollback/removal passes |
| Final staging | Normal and stripped payloads contain 11 files each; helper/policy approval hashes match final staged bytes |
| Runtime independence | Staged normal executable loads from a temporary prefix/unrelated directory without development environment variables. External AeroQt supplied separately; no checkout path or missing library in loader resolution. Headless loader evidence only |
| Live application-file installation | Exact normal 11-file payload installed; every installed file/mode matches manifest. Existing files backed up; shared AeroQt unchanged |
| Payload manifest SHA-256 | `e020192274e21fbce7009656a380c0b3a3903fe15735f53cd0d86101a1b2af4d` |
| Installed terminal identity | Absolute installed `--version` matches identity from an unrelated directory; PATH lookup resolves `/home/siru/.local/bin/control` |
| Installed menu / GUI launch | **Not run**. Desktop metadata names the exact executable; this does not prove menu or graphical launch |
| Fresh sign-in launch | **Not run**; coordinate session restart with user |
| Primary-page layout | 13 Qt fixture rows pass; all 32 captures reviewed at 1×, in 1000×650 and 1920×1080 viewport harnesses; Sound at 640×560. Readable fields/errors/actions and representative focus/scroll reachability verified. Actual installed MainWindow remains RC-4 |
| Feedback materials | [Template supplied](mvp-rc-feedback.md); trial has not started |

Shareable logs, source/payload manifests, loader/dependency records, deployment
results, representative captures and all capture hashes are in
[the evidence directory](evidence/mvp-rc1/README.md). Raw KDE configuration,
device details and application backup bytes remain private.

The immutable source archive is retained at
`build/rc1-bundle-2/source/source.tar.gz`. It captures the pre-qualification
worktree, including modified/untracked inputs and Batch 25. This record and other
result documents are published after testing; they are not compiled inputs.
Archived code, tests, build instructions and shipped payload bytes remained
unchanged through validation and installation. The older parity backlog remains
historical evidence; these results add no whole-module parity claim.

## Mandatory installed workflows

All rows require observer, date/build, baseline, requested change, actual effect,
saved/readback state, reopening, restoration and persistence where applicable.
The table remains open until those observations are recorded.

| ID | Scenario | Result / blocker |
| --- | --- | --- |
| W1 | Menu launch, six named search destinations, Back/Home/keyboard and pending work | Not run |
| W2 | Playback volume/mute and supported default-output change; restore baseline | **Not run — blocked:** only one available physical sink in the passive inventory. A second real output is required; user coordination pending. Baseline muted at 0%, no setter used |
| W3 | Discard and apply one supported pointer/scroll preference; physical effect and restore | Not run |
| W4 | Selected desktop local picture/fill; visible result and restore; other desktops preserved | Not run |
| W5 | Night Light preview/Stop and one persistent setting; visible effect and restore | Not run |
| W6 | Applicable power timeout; effective behavior and restore; charging policy preserved | Not run; timeout wait must be coordinated |
| W7 | Supported approved Aero tuning; visible effect and restore | Not run |

## Modified-provider and optional recovery checks

| Scenario | Result |
| --- | --- |
| Font role: effect, documented consumer restart, readback/persistence and restore | Not run |
| Keyboard repeat preference: actual effect, persistence and restore | Not run |
| Title-bar action: actual behavior, persistence and restore | Not run |
| Default terminal via kdeglobals: supported consumer adoption and restore | Not run |
| Common application shortcut: consumer adoption/invocation and restore | Not run |
| Display Keep / Revert / timeout recovery | Not run; production mutations withheld |
| Active-network Keep / Revert / timeout recovery | Not run; connectivity/active-profile mutations withheld |
| Secondary editor launch/read smoke | Not run |

## Charging preservation

The pre-build passive baseline records conservation mode `1`, charge type
`[Long_Life]` and saved UPower preference `1`. These three policy paths are
monitored separately from dynamic charge percentage and ordinary KDE files.
This does not identify the original October 18 disabling actor or prove that
fixture testing excludes indirect desktop effects. Live W6 acceptance needs its
own before/after record and coordinated restoration.

All three charging-policy fingerprints remain unchanged across recorded
checkpoints. Of 59 monitored paths, 57 matched before installation and 55 after.
The application desktop entry was intentionally replaced; KSycoca cache drift
first appeared after installation. KNightTime/KWin state drift preceded
installation. Cache/state drift remains unattributed; private bytes are retained,
and indirect effects are not excluded.

No live setting mutations, locking, connectivity changes, power timeout tests or
session restarts have been performed. Earlier fixture failures were repaired and
the complete suite rerun on the final snapshot. Passing fixtures do not qualify
live effects or persistence. Coordinate those checks with the user. The five-day
usage trial has not started.

## Application rollback

Private persistent journal: `/home/siru/.control-panel-rc-deployment`.
It retains original application files and each managed path's prior state. Keep
it for upgrades, rollback and removal. To restore the previous application files:

```bash
python3 tools/rc_bundle.py rollback --state /home/siru/.control-panel-rc-deployment
```

The tool refuses intervening edits before changing managed files. This restores
application files only; it does not undo preferences changed while using it.
