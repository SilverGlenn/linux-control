# Working on Linux Control Panel

## Product and current scope

This is a native Linux Qt/KF6 Control Panel with a Windows 7 / custom Aero UI.
The immediate goal is a reliable local candidate for everyday feedback on the
owner's KDE laptop. Development, builds and integration stay on that laptop;
the owner cancelled the Cloud migration. Full KDE System Settings parity comes
later. Keep the custom
Aero theme throughout; do not introduce other theme choices or ship embedded KDE
settings screens as the finished interface.

Read these before release, installer, updater or cloud work:

- [Release gates](docs/mvp-release-candidate.md)
- [Current local candidate and acceptance record](docs/local-acceptance.md)
- [Historical manual-update RC2 record](docs/manual-update-validation.md)
- [Historical RC1 qualification record](docs/mvp-rc1-qualification.md)
- [Installation and rollback](docs/local-rc-install.md)
- [Manual update workflow](docs/manual-updates.md)
- [Local development and release workflow](docs/local-development.md)

The qualification record identifies a particular artifact. Historical fixture
passes are not results for a changed checkout or proof of live desktop behavior.

## Preserve work and desktop safety

- Inspect `git status` first. Preserve existing modified and untracked files,
  including Batch 25 and the MVP work. Never reset, clean, stash, or overwrite
  other work to obtain a clean build. Use a separate checkout/build directory.
- Keep provider/version guards, root trust checks, conflict detection and the
  fixed display/network mutation withholding. Unsupported actions must remain
  disabled with an accurate explanation.
- Settings jobs must survive page navigation. Preserve Apply/Revert, asynchronous
  ownership, readback and exit guards; an updater must wait for the app to close.
- Tests must use private HOME/XDG paths, private session/system bus endpoints and
  fake providers. Do not run disruptive live desktop tests without coordinating
  with the owner. This includes charging/power writes, audio effects, connectivity,
  display changes, locking, sign-out and reboot.
- The charging-limit incident's original actor is unproved. Do not attribute it
  to a particular change or repeat a charging setter. Preserve the charging
  policy; physical behavior requires separately coordinated laptop evidence.
- Install only the bounded user-local application payload. AeroQt, Aero assets,
  KDE preferences and system packages remain external. Do not add system-wide
  polkit authorization as an updater prerequisite.

## Builds, releases and evidence

CMakeLists.txt declares dependencies. Use the laptop's supported KDE stack and
record its external AeroQt/development inputs; a fresh clone does not contain
those dependencies. Follow the local workflow instead of weakening minimum
versions or guards. Cloud provisioning is not a prerequisite.
Compile the intended absolute install prefix into each release before building.

Build release artifacts from the frozen source archive, run the complete isolated
suite, then stage and validate those exact bytes. Use `tools/rc_bundle.py` for
bounded transactional deployment and preserve the original deployment journal.
Do not add GitHub Actions/workflows or require a paid API/CI service for updates.
Build and validate releases locally. GitHub Releases can distribute them to the
manual updater without remote build compute. The updater consumes releases only
after explicit actions in System and Security > Linux Update. The
owner replaced the automatic-update proposal with manual checks/downloads and
Close and install. Do not add timers, startup polling or automatic installation.

Record build/source/payload identities, dependency inputs, commands, results and
remaining live gates. Never replace recorded historical hashes with current
hashes to imply a new validation pass. A new build invalidates affected acceptance
evidence. Never label fixture-only results as qualified hardware behavior.

If using subagents, use only GPT-6.1 Sol at medium/high or GPT-6 Luna at
high/xhigh, within the environment's concurrency limit. Assign shared-file and
integration ownership before parallel edits.

## Code Review Rules

- Flag any updater path that executes unverified downloads, bypasses dependency
  guards, replaces files during app/jobs activity, or loses rollback state.
- Flag release/qualification claims that lack evidence for the exact artifact,
  and tests that can reach the real user's configuration or desktop providers.
