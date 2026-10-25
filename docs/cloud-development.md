# Preparing a Codex Cloud environment

Historical proposal, superseded by the owner's decision to keep development and
builds on the laptop. This is not a release prerequisite. Follow
[the current local workflow](local-development.md); the original proposal remains
below for reference.

This is the setup contract for a fresh agent, not a claim that a Cloud build has
already passed. Read [AGENTS.md](../AGENTS.md) and the
[handoff prompts](codex-cloud-handoff.md). A Windows 7 VM is unnecessary: the
application and tests are native Linux. Physical KDE/hardware acceptance stays on
the laptop and must be coordinated.

## Source checkpoint

At the start of the updater design, local HEAD was
`336bc91882cdab8a1625e95de173160a607974d0`, with preserved uncommitted Batch 25 and
MVP changes. The installed candidate is
`0.1.0-rc.1+7584b7fda4e74a21`. Neither HEAD alone nor that installed build ID
identifies the current, subsequently documented working tree.

Before moving development to Cloud, review/checkpoint all intended source,
tests, tools and documentation to `SilverGlenn/linux-control`. Check for private
data before adding evidence. Build directories, raw desktop baselines, original
file backups and credentials stay local. Confirm the remote commit actually
contains `tools/rc_bundle.py`, `src/release/`, the MVP docs and Batch 25 changes.
Do not reset or discard local work to match the remote.

Cloud environments can save installation/startup configuration and prepared
dependencies, with separate task workspaces. Commit durable instructions and
source; saved environment state is not source control. Configure network access
and credentials explicitly in the environment settings.
[Official environment documentation](https://learn.chatgpt.com/docs/environments/cloud-environments).

## Dependencies that must be made reproducible

Use `CMakeLists.txt` as the declaration and
[local installation requirements](local-rc-install.md#build-inputs-and-dependencies)
for the complete list. The recorded laptop stack includes Qt 6.10.2, KF6 6.24.0
and KNightTime 6.6.5; the adapter needs C++20. Those versions are a starting
compatibility target, not evidence that a generic Cloud image supplies them.

| Input | Required preparation |
| --- | --- |
| OS/toolchain | Compatible x86_64 Linux userspace/toolchain and runtime ABI; record image identity, package origins and exact versions |
| Qt/KF6/KNightTime | Install compatible development and test packages, including Positioning/Holidays; preserve all provider guards |
| AeroQt | Obtain the owner's compatible source revision/patches and build recipe, or an approved versioned development/runtime artifact with hashes and ABI; upstream URL alone does not establish equivalence |
| Other development inputs | CUPS, fontconfig, xkbcommon, OpenSSL, zlib, pkg-config, CMake, compiler; Qt Test/QML, Python 3.9+ and private D-Bus tools for tests |
| Custom Aero assets | Record external theme/plugin identities needed by the runtime; keep them external to the managed application payload |

The local setup uses ignored extracted development packages under
`build/deps/nighttime/root/usr` and `build/deps/cups/usr/include`, and AeroQt under
`/home/siru/.local`. A fresh clone does not contain those inputs. The checked-in
evidence records identities, not their complete distributable contents. Resolve
and record a lawful, repeatable source for each missing dependency. Stop with a
specific dependency report if unavailable; do not fabricate a successful setup,
link to arbitrary local caches or lower CMake requirements.

The cloud-built laptop artifact must compile `/home/siru/.local` as its install
prefix even if the Cloud task uses a different home. Stage with `DESTDIR`; never
install into the cloud user's real prefix to simulate the target. CMake install
prefix overrides after compilation do not rewrite compiled helper paths.

## Release driver requirements (U1)

The packaging stage now exists in `tools/make_update_release.py`, and private
fixture execution uses `tools/run_isolated_tests.py`. Complete and qualify the
Cloud build/release driver rather than reconstructing session transcripts. It must:

1. Resolve an exact reviewed main commit and confirm a clean release checkout.
   Include intended untracked work by checkpointing it first, not by dropping it.
2. Freeze source with `tools/rc_bundle.py source`. Safely unpack and build that
   archive in a separate directory using its generated identity preload. The
   local installation guide's configure example uses the checkout; a release
   driver must instead bind compilation to the frozen bytes.
3. Configure Release, `BUILD_TESTING=ON` and the target absolute prefix. Record
   compiler/CMake/package/library identities, all explicit inputs and commands.
   Use bounded build parallelism/memory appropriate to the Cloud environment.
4. Run the **complete** isolated CTest suite with retained logs. Use the repository runner, which creates private HOME/config/cache/data/runtime paths, an offscreen
   UI, private outer session bus without real service activation, private system
   bus address and explicit fake providers. Existing CTest per-test fixtures must
   keep working inside it. A plain `ctest` invocation is not proof of that isolation.
5. Stage the final payload, including install-time RPATH/helper-policy identities.
   Generate the bundle from the staged prefix. Record external dependencies but
   do not copy or own AeroQt/themes/system packages. Exercise exact-payload staged
   install, upgrade, rollback, removal and interrupted recovery in private roots.
6. Check runtime loading and provider-free `--version` from an unrelated directory
   without development loader paths. Retain all ELF/runtime identities and source,
   archive and payload hashes. Recheck frozen inputs after validation.
7. Generate the strict release descriptor and evidence. Publish only these exact
   tested bytes through the [manual update contract](manual-updates.md). A changed
   input or rebuilt binary requires fresh evidence and identity.

The recorded local result was **107/107 isolated suites** for the old frozen
artifact. It is useful historical evidence, not a fixed expected test count or
a result for a new Cloud build. New tests should increase the discovered suite;
do not filter failures to reproduce the old count.

## Access and qualification boundaries

Use a coding environment without release-writing credentials for arbitrary PR
work. A separate trusted release environment may publish reviewed main commits
using only the repository permissions it needs. Confirm upload/verification and
network-secret behavior on that actual environment; do not assume a GitHub source
connection automatically grants release publishing.

The laptop consumes immutable release assets; no Cloud task needs laptop SSH,
desktop buses, power controls, saved KDE files or charge-policy credentials. No
GitHub Actions or paid API key is required. The owner requests manual checking,
downloading and Close and install inside Linux Update: no laptop timer, startup
polling or automatic installation. A Cloud release task publishes tested assets.

Cloud fixtures can validate logic, UI, packaging and recovery. They cannot prove
physical charging limits, real audio routing, pointer feel, live Night Light,
wallpaper, suspend, sign-in persistence or installed menu behavior. Keep W1–W7 and
modified-provider acceptance open until the exact installed build is observed on
the laptop. Preserve display/active-network mutation withholding and the existing
user-local privileged-helper limitations.
