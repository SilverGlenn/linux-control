# Local RC bundle and application-file rollback

This workflow prepares the Ubuntu KDE/Aero candidate described in the
[release gates](mvp-release-candidate.md). A successful staged installation is
fixture evidence. It does not qualify desktop launch, physical setting effects,
sign-in persistence or the seven mandatory workflows. Keep those gates open until
they have been observed on the installed application.

The bundle owns Control Panel's executable, three helpers, desktop entry, two
polkit policies and files under `share/linux-control/`. The final payload manifest
is authoritative; its file count is not fixed. It never installs or takes ownership
of AeroQt, Aero themes, KDE preferences or system packages. Files copied under the
user's prefix do not enable system polkit authorization. Time-server and login-theme
editing remain unavailable because their existing root-ownership/trust checks are
preserved. The existing package-mutation pages remain unavailable on Ubuntu
without their supported Arch tools. The separate read-only APT preview remains
available; APT mutation support is outside this candidate.

## Build inputs and dependencies

Configure a separate Release directory with the absolute prefix
`/home/siru/.local` **before compiling**. The application compiles in its helper
paths. Do not attempt to change that identity with `cmake --install --prefix`.
The installed desktop entry uses that prefix's absolute `bin/control`, so menu
launch does not depend on a different PATH.

The project requires a C++17 compiler, with C++20 support for the KNightTime
adapter; CMake; pkg-config; Qt Widgets, Multimedia, Concurrent, DBus, OpenGL,
UiPlugin, Network, XML and Positioning; KF6 WidgetsAddons, WindowSystem,
FrameworkIntegration, Config/ConfigGui, Service, GlobalAccel, XmlGui, IconThemes,
CoreAddons, KIO and Holidays; KNightTime; AeroQt; OpenSSL; zlib; fontconfig;
libxkbcommon; and CUPS development inputs. Test builds also require Qt Test/QML
and Python 3.9 or later for the release-tool fixtures. CMakeLists.txt remains
the exact dependency declaration, including minimum versions.

At the October 25 starting snapshot, Qt 6.10.2, KF6 6.24.0, KNightTime 6.6.5,
KWin/Plasma Desktop/PowerDevil 6.6.6 and kglobalacceld 6.6.5 were present. These
are evidence to refresh, not permission to weaken the provider guards. AeroQt's
local CMake package reports `1.0`, while its imported library is named
`libAeroQt.so.6.0.1` with SONAME `libAeroQt.so.0`; record its path, hash and
SONAME rather than treating the filename as an authoritative package version.

The existing Debug cache additionally uses extracted **development** inputs:

- `build/deps/nighttime/root/usr` supplies KNightTime, KF6Holidays and
  Qt6Positioning headers/CMake configs.
- `build/deps/cups/usr/include` supplies CUPS headers; CUPS links to the system
  `/usr/lib/x86_64-linux-gnu/libcups.so.2`.

The corresponding runtime KNightTime, Holidays and Positioning shared libraries
are already installed under `/usr/lib/x86_64-linux-gnu`. Extracted development
inputs do not establish runtime independence. Record the `.deb` hashes and
resolved development prefix when reusing them; alternatively provide compatible
installed development packages. Do not copy those development directories into
the application's managed payload. AeroQt remains an external dependency under
`/home/siru/.local/lib`.

## Freeze a candidate and stage it

Run commands from the checkout. Review and finish implementation before freezing.
The source command reads Git's tracked and nonignored untracked file list,
including existing modified and Batch 25 inputs; it does not modify Git. A
tracked deletion is represented by absence in the complete archive. It records
file bytes/modes and symlink targets, normalizes archive timestamps/owners and
writes the source manifest, deterministic archive, identity and CMake preload.
Output inside the checkout must be ignored, avoiding self-reference.

```bash
python3 tools/rc_bundle.py source --root . --output build/rc1-source --version 0.1.0-rc.1
```

Review the exact `source-manifest.json` input list before using its identity.
The full SHA-256 identifies the included content; HEAD and the short build ID
alone are not substitutes. A changed input requires a new freeze and build.
Never edit the generated identity merely to make an old artifact match new code.
Unrecorded development builds visibly identify themselves as unqualified.

For this machine's existing extracted development inputs, an example configure is:

```bash
cmake -S . -B build/rc1-release -C build/rc1-source/build-identity.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/home/siru/.local \
  -DCMAKE_PREFIX_PATH=/home/siru/Projects/linux-control/build/deps/nighttime/root/usr \
  -DAeroQt_DIR=/home/siru/.local/lib/cmake/AeroQt \
  -DQt6Positioning_DIR=/home/siru/Projects/linux-control/build/deps/nighttime/root/usr/lib/x86_64-linux-gnu/cmake/Qt6Positioning \
  -DCUPS_INCLUDE_DIR=/home/siru/Projects/linux-control/build/deps/cups/usr/include \
  -DCUPS_LIBRARIES=/usr/lib/x86_64-linux-gnu/libcups.so.2
cmake --build build/rc1-release -j2
# Run the complete isolated suite with the reviewed fixture runner here.
DESTDIR="$PWD/build/rc1-stage" cmake --install build/rc1-release
```

The complete isolated suite must use the project's approved isolation environment
and test runner. Retain
its logs, configuration/dependency identities, source hash and final binary hashes.
Check source inputs again after validation. The staged payload is the installed
prefix **inside** DESTDIR; pass that directory to the bundle command:

```bash
python3 tools/rc_bundle.py bundle \
  --stage build/rc1-stage/home/siru/.local --prefix /home/siru/.local \
  --source build/rc1-source --output build/rc1-bundle \
  --dependency /home/siru/.local/lib/libAeroQt.so.0 \
  --dependency /usr/lib/x86_64-linux-gnu/libKNightTime.so.0 \
  --dependency /usr/lib/x86_64-linux-gnu/libKF6Holidays.so.6 \
  --dependency /usr/lib/x86_64-linux-gnu/libQt6Positioning.so.6
```

Record other resolved runtime/development libraries and downloaded `.deb` files
with additional `--dependency` arguments or retained qualification evidence.
Dependency records hash the resolved external file and do not copy or own it.
The bundle rejects files outside the bounded application payload, candidate
symlinks, changed bytes/modes and a PATH-only desktop launcher. The payload
manifest includes SHA-256 for every distributed binary. Keep this bundle intact;
deployment reads the final staged bytes rather than rebuilding or stripping them.

## Exercise reversible deployment in a private directory

Use a new private root and a separate persistent state directory:

```bash
python3 tools/rc_bundle.py install --bundle build/rc1-bundle \
  --staging-root build/rc1-install-fixture --state build/rc1-install-state
python3 tools/rc_bundle.py rollback --state build/rc1-install-state
```

Repeat installation, upgrade with a second bundle, rollback and removal against
fixtures containing old executable bytes, changed modes, pre-existing desktop
symlinks, unrelated files and deliberate intervening edits. The focused Python
fixtures cover these cases and interrupted deployment recovery. They run only
inside temporary directories.

An install/upgrade journals exact pre-existing file bytes/modes or symlink targets,
then replaces ordinary application files atomically one file at a time. It
retains the original objects and all deployment histories in the state directory.
`rollback` restores the immediately preceding installation state; on the first
installation that means the pre-RC objects. `remove` restores the original object
from when each managed path first became owned, including paths added by an
upgrade. Files originally absent are removed. Empty parent directories and backup
journals are retained. A removed path from a newer payload remains tracked as
absent until ownership is rolled back or removed.

Before upgrade, rollback or removal changes any file, **every** managed
fingerprint must still match, including absent paths. Any intervening content,
mode or symlink change refuses the operation and lists the conflict. Resolve that
conflict deliberately; do not force replacement or discard the original backup.
Symlink ancestors, path escapes, directories in place of managed files and shared
library/theme payloads are refused. Pre-existing symlink leaves are preserved as
links without reading their targets. A private state-directory lock prevents two
deployment commands from sharing one journal concurrently. These file operations
do not make a multi-file deployment atomic against unrelated external writers.

If interrupted, a durable `pending.json` remains. New operations refuse to proceed
until recovery. Recover checks every affected path against the exact original or
the transaction's intended replacement before restoring the pre-operation state:

```bash
python3 tools/rc_bundle.py recover --state build/rc1-install-state
```

An intervening edit also blocks recovery. A successful install is not permission
to discard its persistent state directory: removal and rollback need its backups.
Application-file rollback does not undo desktop settings changed while using the
application.

Verify runtime dependencies separately. A staged app's `$ORIGIN/../lib` points
inside the private root, which intentionally lacks externally owned AeroQt.
For a launch probe, supply a private copy of that dependency in a separate fixture
step, record its hash, and remove that fixture manually when finished. Do not add
it to the application's managed manifest. Inspect loader resolution without
development-only `LD_LIBRARY_PATH`, checkout/build paths or source assets; repeat
the headless `--version` query from an unrelated directory. Use the generated
installed helper paths for display recovery qualification. No staged result proves
the real menu or a later sign-in works.

## Activate the reviewed bundle only after the preparation gates

After RC-1 through RC-3 and final isolated/staged checks pass, the integration
owner can activate the **same** bundle by omitting `--staging-root` and using the
canonical persistent private state directory `/home/siru/.control-panel-rc-deployment`.
Reuse that existing journal for upgrades; create it only for a first installation.
It remains outside `/home/siru/.local`. Without staging,
the tool only permits this user's exact `~/.local` prefix. Keep the old executable
and desktop entry backups until live acceptance and the trial are finished.
Use that same state directory for subsequent upgrades, rollback or removal.
Do not reuse the staging journal for live activation.

Run `/home/siru/.local/bin/control --version` with no graphical session if useful;
`-v` is equivalent. `--help`/`-h` also avoids Qt/provider startup. These switches
must be the sole argument; mixed queries return exit status 2 before starting Qt.
Compare the result with the visible shell build label, desktop metadata, payload
hashes and source manifest. Complete RC-4's installed menu/terminal, restoration,
physical-effect and later-sign-in checks deliberately with the user's session.

The current installer and installed app share a destination lock; keep all app
instances closed for the one-time migration from the older non-locking RC1.
An interrupted transaction blocks provider startup until independent recovery.
For the separate trusted runtime and manual Linux Update actions, follow
[the manual update procedure](manual-updates.md). No timer is installed.
