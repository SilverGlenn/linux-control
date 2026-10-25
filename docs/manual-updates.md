# Manual Control Panel updates

The owner selected this workflow on 25 October 2026, replacing the automatic-update
proposal. The source implements manual UI, a verified consumer, packaging and
app/installer locking. RC2 and its trusted updater are now installed; a real
published-release update and installed acceptance remain separate gates. See the
[current candidate record](manual-update-validation.md). Development and builds
stay on the laptop; the owner cancelled the Cloud migration.

## User workflow

Open **Control Panel > System and Security > Linux Update**. The first section is
**Control Panel updates**. Existing operating-system package updates and Ubuntu
Package Preview remain independent.

1. **Check for Control Panel updates** contacts GitHub only when clicked. Missing
   setup, no published releases, incompatibility and verification failure are
   distinct from a successfully checked up-to-date result.
2. **Download update** verifies that exact release and prepares a private receipt.
   It does not replace installed files. Checking/downloading survives navigation.
3. **Close and install** is the explicit installation action. The shell refuses
   while jobs, settings drafts, navigation decisions or retained imports remain.
   Apply/discard them first. The trusted detached helper then waits for every
   installed app instance to close. An ordinary Close never schedules installation.
4. Reopen normally afterward. **View last installation result** reads saved local
   status without going online. Another still-open instance causes a safe timeout;
   the update remains prepared for an explicit later attempt.

There is no timer, startup polling, automatic download/install, forced close,
sign-out or reboot. Updating app files does not change KDE or charging settings.

## Trusted runtime and deployment

`ControlPanelUpdateService` owns bounded asynchronous helper jobs; LinuxUpdatePage
uses the existing Windows 7 styling and keyboard controls. The private helper
`tools/control_panel_update.py` implements explicit `check`, `prepare`, `apply` and
local `status`. Production has no alternate-home/target/verifier CLI bypass.

The bootstrap copies three reviewed Python modules and the installed GitHub CLI
into `~/.control-panel-updater/runtime/`: directory 0700, Python files 0600, verifier
0500. Configuration is `~/.config/linux-control/updater.json` (0600), pinning
`SilverGlenn/linux-control`, repository ID `1421857804`, channel `laptop-candidate`,
the target and verifier. That public repository ID was checked read-only on the
design date; every check revalidates it. Isolated Python loads validated siblings,
not ambient modules, downloaded installers or executable update hooks.

The consumer verifies an immutable release and BOTH consumed assets using
`gh release verify` / `gh release verify-asset` with an explicit repository/tag.
Attestations establish provenance, not code quality or truthful test evidence.
[Immutable releases](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases),
[verification](https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/secure-your-dependencies/verify-release-integrity).

Checks repeat before application. Archives are bounded USTAR gzip with only
expected regular files/directories; traversal, links, metadata bombs and extra
files are refused. Receipts bind the attested descriptor/archive and cannot be
replayed. Failed candidates are quarantined. The sequence high-water mark survives
rollback. Exact external dependency hashes conservatively reject drift, including
some ABI-compatible package changes; broader ABI/provider contracts require later
explicit implementation. Retain existing runtime provider/root trust guards.

The app holds a shared lock before providers through teardown; all deployment
commands require the same exclusive lock at
`~/.control-panel-rc-deployment/application.lock`. Production uses the original
canonical journal. Pending transactions block provider startup. `rc_bundle.py`
checks managed fingerprints, journals per-file replacement and performs conservative
rollback/recovery. It is not a globally atomic directory switch. AeroQt, themes,
preferences and system packages remain external and are never upgraded to make
an incompatible artifact fit.

## Release contract

Build/test an exact reviewed main commit locally, then publish an immutable
GitHub Release when a distributable update is needed. No Cloud build service,
GitHub Actions or paid API service is required. A raw main push is not a published
update. Follow [the local development workflow](local-development.md).

Tag: `laptop-candidate-<positive sequence>-<commit12>`. Set the release's
`target_commitish` to the full commit SHA, not the string `main`. The tag must
resolve to that commit on main ancestry. Create a draft, attach complete assets,
then publish immutably. Consume `release.json` and `control-panel-bundle.tar.gz`;
retain `validation.json` and build/test/runtime evidence too.

Strict descriptor schema 1 has exactly:

```text
schema, repository, repository_id, channel, sequence, tag, commit, build_id,
source_manifest_sha256, payload_manifest_sha256, archive_sha256, archive_size,
target, minimum_updater_version
```

Minimum updater version is 1. Target has exactly architecture `x86_64`, os_id
`ubuntu`, os_version_id `26.04`, session_type `wayland`, prefix `/home/siru/.local`,
and nonempty external_dependencies (`path`/`sha256`, ordered as in the bundle).
Include the full reviewed runtime dependency inventory. Source/payload identities
must match the complete RC bundle. The outer archive uses USTAR, not GNU/PAX;
embedded source.tar.gz remains a regular file.

`tools/make_update_release.py --bundle PATH --checkout PATH --sequence N
--output NEWDIR --validation PATH` is the packaging stage. It checks a clean main
checkout, frozen inputs and matching recorded evidence. The validation record has
schema 1, source_manifest_sha256, payload_manifest_sha256, complete_fixture_suite
with total>0, passed=total, failed=0, not_run=0, staged_deployment_passed=true,
runtime_check_passed=true, qualification="candidate for acceptance".
Packaging does not compile, run tests or independently prove their recorded
results. Qualify the repeatable local release driver and real attestations before
claiming that pipeline works.

## Initial setup and repair

Coordinate the initial migration from RC1: the older binary does not hold the new
lock, so confirm all instances are closed before replacing it. Preserve its
original `~/.control-panel-rc-deployment` journal. After reviewing the runtime,
run the bootstrap with an absolute path to the installed GitHub CLI:

```bash
python3 tools/bootstrap_manual_updater.py --verifier /absolute/path/to/gh
```

It copies reviewed tools, not the app; makes no network request; changes no packages
or settings; and creates no service. Existing different files require review and
explicit `--replace-reviewed`. `--staging-root` rehearses setup privately. The
verifier must support release verification and have usable GitHub access. Do not
copy Cloud credentials to the laptop or grant Cloud agents laptop SSH access.

The source verifier must have trusted ownership and protected permissions. If its
parent directory is writable by a group/others (as on this laptop's `.local`),
bootstrap requires an independently reviewed full SHA-256 via
`--reviewed-verifier-sha256`. This pins copied bytes instead of trusting an unsafe
PATH entry. It does not make a group/world-writable executable acceptable.

With the app closed, independent repair is:

```bash
python3 ~/.control-panel-updater/runtime/rc_bundle.py recover --state ~/.control-panel-rc-deployment
python3 ~/.control-panel-updater/runtime/rc_bundle.py rollback --state ~/.control-panel-rc-deployment
```

Use recover for an interrupted transaction, rollback for a completed upgrade.
Follow conflict diagnostics; never force through intervening edits. These restore
app files, not preferences changed while using a version. Updater upgrades require
a separate reviewed bootstrap; application releases cannot replace its trust root.

## Qualification

Run all discovered fixtures with `tools/run_isolated_tests.py`; retain exact
source/build/payload identities and logs. Fixtures cover manual-only actions,
retained jobs, unsafe inputs, app/installer races, interruption, actual private
deployment/rollback, dependency drift, sequence/replay/quarantine and bootstrap.
Then coordinate a real installed check → download → close/install → reopen → local
result → rollback. Fixture passes do not complete this gate or RC-4/W1–W7. Keep
the second real audio-output requirement and all modified-provider checks. A
five-day trial starts only after qualification, on a single chosen build; changed
code reopens affected acceptance.
