<div align="center">

  # linux-control
  <i>The Windows Control Panel, on Linux</i>

  <p>
    A faithful recreation of the Windows Control Panel built with Qt6, KDE Frameworks, and <a href="https://gitgud.io/atmk/libaero-qt">libAeroQt</a>, backed by real system data via <code>pacman</code>/<code>pkexec</code>/<code>ufw</code>. Best enjoyed with <a href="https://github.com/aeroshell-desktop/aerothemeplasma">AeroThemePlasma</a>.
  </p>

</div>
<br>

> [!WARNING]
> **Very much a work in progress.** Several features are placeholders or don't do anything yet, the code is janky and buggy in places, and it needs a lot more work before it's daily-driveable. Expect rough edges until about August.

> [!NOTE]
> **Built for CachyOS / Arch Linux.** Some features (package updates, AUR reinstall, driver-adjacent firewall config) depend on `pacman`, `yay`, and `ufw`. Other distros may need minor adjustments.

## Installation

### Arch / CachyOS (AUR)

```bash
yay -S linux-control
```

### Pre-built binaries

Download the latest archive for your architecture from the [Releases](https://github.com/actuallyaridan/linux-control/releases/latest) page and extract it somewhere in your `$PATH`:

```bash
tar -xf linux-control-x86_64.tar.gz
sudo mv control libAeroQt.so* /usr/local/bin/
```

`libAeroQt.so` is a separate runtime dependency (not statically linked), so it needs to ship alongside `control`.

## Building

### Arch / CachyOS

```bash
sudo pacman -S qt6-base qt6-multimedia cmake kwidgetsaddons kwindowsystem openssl zlib
cmake -B build
cmake --build build -j
./build/control
```

You'll also need `libAeroQt.so` on your system, built from [libaero-qt](https://gitgud.io/atmk/libaero-qt). Place the resulting `libAeroQt.so*` next to `build/control` (or install it to your library path).

Native appearance and font editors also require Qt XML, KF6 Config (including
ConfigGui), IconThemes, CoreAddons, fontconfig development files and pkg-config.
The native Default Programs provider additionally requires KF6 Service.
Shortcut editing and keyboard previews require KF6 GlobalAccel, KF6 XmlGui and
libxkbcommon development files.
The isolated wallpaper scripting tests additionally use Qt QML.

## Runtime dependencies

On Arch, you need these installed separately.

| Tool | Purpose |
|------|---------|
| `pkexec` | Privilege escalation for package installs/uninstalls |
| `pacman` | Update checks, installed-package listing, program uninstall |
| `yay` | *(optional)* Reinstalling AUR/foreign packages from Programs & Features |
| `ufw` | Firewall status/rules shown on the Firewall page (read-only) |

## Internet time

The Date and Time applet detects Chrony and systemd-timesyncd. It reports
automatic synchronization separately from whether the system clock is actually
synchronized. Failed or ambiguous service queries appear as unknown.

Changing the preferred server uses the included `controlpanel-time-helper`,
which validates the server again after administrative authorization and writes
configuration atomically. It does not execute a shell. For Chrony, it adds a
preferred source alongside the existing sources in `/etc/chrony/sources.d`;
that directory must already be included by `/etc/chrony/chrony.conf`.
Selecting **System defaults** removes only Control Panel's own server preference.
**Update now** requests new measurements; it does not force a clock jump or
claim that synchronization has succeeded.

Server editing is enabled only when the helper and its ancestors are owned by
root and are not writable by other users. Configure a system installation prefix
before building and install the helper and polkit policy together:

```bash
cmake -S . -B build-system -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build-system -j
sudo cmake --install build-system
```

Make AeroQt discoverable to CMake and the runtime loader as described above.
The helper itself depends only on system Qt libraries. A build installed under
your home directory can still toggle supported synchronization and request fresh
measurements through the system tools; server editing remains unavailable there.

## Current implementation limits

Programs and Features, Linux Update, and Installed Updates currently require
pacman. On Ubuntu and other systems without pacman they report unavailable;
APT integration is planned. A failed update check is not shown as up to date.

Action Center does not monitor automatic updates, backups, antivirus protection,
or system maintenance. The firewall page shows recognized UFW configuration,
not proof that rules are currently enforced. Missing data is shown as unknown.

Unsupported task links, Sound scheme/communications controls, additional
taskbar clocks, and decorative menus are disabled. Sound
and Date and Time use Close-only outer dialogs because their supported changes
apply through explicit actions or subdialogs. Personalization is restricted to
the approved custom Aero theme and its supported tuning controls.

## Settings and background work

Personalization stages Aero customization and restoration. **Apply** checks the
saved baseline, approved assets, runtime and write policy, then reads back saved
values before reporting success. **Revert** discards pending choices and reloads
the current settings. External changes preserve pending choices and require
Revert to resolve conflicts. Navigation and closing offer Apply, Discard, or
Cancel for pending settings. Failed Apply keeps the page open with its error.

Update checks, package transactions, installed-program queries, device/history
reads, power-profile writes, and performance assessments have owners outside
their pages. Navigating away preserves their work; returning attaches to their
progress or result. Saved device/history reads are marked while refreshing.
The window's background-work strip and **Details** show running process jobs
and retained results. Control Panel waits for active work before allowing exit;
package transactions cannot be cancelled by navigation or closing the window.
This preserves work within the running application, not across process restarts.

Power profiles and Sound device/application settings change through explicit
immediate actions. Their presentation does not imply that Revert can undo them.
The settings-session infrastructure covers Aero customization and restoration,
desktop backgrounds, KDE font/rendering preferences, screen resolution,
PowerDevil timeouts, Night Light and Day and Night, network connections, and the KWin title-bar preference. Native editors share
staging, conflicts, navigation protection, and application-owned operation state.

## Native Night Light controls

Night Light stages enablement, Constant/Dark-Light mode and independent day/night
color temperatures in the Aero interface. Apply uses KDE native notifications and
checks saved preferences plus available public runtime observations. Defaults
follow native inherited/global/no-op semantics; explicit temporary Preview and
Stop do not save the draft. See [filter scope and verification limits](docs/kde-batch19-night-light.md).

The linked **Day and Night** page stages custom times, transition duration and
automatic/manual location, with an offline map and native calculated forecasts.
It distinguishes the editor's global defaults from the daemon's saved inputs.
Opening the page never requests location; **Get location once** is explicit and
its sample stays temporary until separately staged and applied. Real provider
authorization, runtime adoption and persistence remain unverified. See
[Batch 20 scope](docs/kde-batch20-day-night.md).

Night Light and Day and Night now save through persistent worker jobs and share
file reservations with Screen Locking, including KDE's user globals. Slow saves
keep their drafts busy until actual completion. Native Notify remains a
broadcast without an adoption acknowledgement. Other providers still need
migration. See [Batch 24 lifecycle scope](docs/kde-batch24-shared-file-jobs.md).

## Native Sound controls

**Sound** offers native Playback, Recording and Applications tabs for device
defaults, volume, mute and application routing. Properties includes reported
ports, sound-card profiles and per-channel levels. Volume changes preserve the
existing balance. Controls use explicit immediate actions; Close does not undo
them. Reads and writes survive closing and reopening the dialog.

The asynchronous PulseAudio/PipeWire provider checks stable object identity,
current target values and readback. Failed reads disable stale controls;
uncertain changes lock further writes for that server session. Unsupported
choices remain read-only. Sound events, automatic call ducking, meters/test
sounds and advanced policy remain unfinished. Audio discovery does not play or
capture audio. See [Batch 9 scope and evidence](docs/kde-batch9-audio.md).

## Native Mouse and Touchpad

**Mouse and Touchpad Properties** stages supported device buttons, pointer
speed/acceleration, scrolling, tapping, dragging and enablement. Controls follow
KWin's actual per-device capabilities. Apply checks the baseline again and
verifies reported values; partial or uncertain outcomes remain explicit.
Restore device defaults stages only exported KDE defaults, leaving the
config-driven scrolling-speed default unchanged. Pending choices survive
navigation. The current provider targets KWin 6.6.6 Wayland.
See [Batch 10 scope and evidence](docs/kde-batch10-input.md).

## Native Keyboard and Default Programs

**Keyboard** stages repeat behavior, delay/rate, startup Num Lock, model,
ordered layouts/variants, layout policy, advanced XKB options and layout-change
notifications. It follows KWin 6.6.6 Wayland semantics, including clearing layout
overrides when custom layouts are disabled. The unused Wayland layout-loop
subset stays read-only. Native previews show compiled symbols for every group and level, with physical
geometry for PC 101/102/104/105 models and an explicit key-list fallback for other
models. See [Batch 11](docs/kde-batch11-keyboard.md) and
[Batch 13](docs/kde-batch13-shortcuts.md) for scope and evidence.

**Default Programs** stages KDE's twelve application roles and compatible MIME
or protocol associations. Restore defaults selects installed, compatible KDE
recommendations. The backend preserves unrelated settings, respects policy,
checks file/application identity and verifies saved preferences; running-app
and service-cache adoption remain unverified. It never launches the selected
application or forces a cache rebuild. See
[Batch 12 scope and evidence](docs/kde-batch12-default-apps.md).

Both pages share Apply/Revert, external-edit conflicts, navigation protection
and application-owned jobs. Their native Aero controls replace the corresponding
development KCM launchers.

## Native global shortcuts and keyboard preview

**Keyboard Shortcuts** discovers the daemon's registered components, contexts
and actions. It stages alternative sequences (up to four chords), individual or
all defaults, and explicit conflict reassignment without silently stealing keys.
The Windows 7 recorder uses KDE's window-scoped Wayland inhibition; offscreen
tests cover recording and lifecycle, while real compositor inhibition remains
unverified. Apply checks current bindings, policy and disk conflicts and reads
back native daemon state and persistence. Inactive application bindings apply
when those applications start. On audited KWin 6.6.6, configured runtime layouts
also expose unassigned per-layout bindings. Apply registers only those verified
layout identities and waits for a targeted KWin reload acknowledgement. Other
versions and unavailable layout capabilities receive an explicit limitation;
physical shortcut invocation remains unverified.

**Application Shortcuts** separately edits KDE's common in-application actions,
including Copy, Open and Save. Its category tree, alternatives, defaults and
conflict reassignment have their own persistent draft. Fresh layered reads
respect system configuration policy; Apply preserves unrelated preferences,
checks external edits, independently reads back saved bindings and sends KDE's
native change notification. Application-specific overrides are preserved, and
running applications may need to restart to adopt the common bindings.

The native **Keyboard Preview** compiles the staged model, layout, variant and
options in-process and shows key symbols, levels and modifier combinations.
It never switches the active layout. Additional model geometry and live
acceptance remain on the full parity backlog. See [Batch 13](docs/kde-batch13-shortcuts.md),
[Batch 14 application shortcuts](docs/kde-batch14-standard-shortcuts.md) and
[Batch 14 layout activation](docs/kde-batch14-layout-shortcuts.md) for scope
and evidence.

Both shortcut pages can review and export native KDE `.kksrc` schemes. Import
shows selected assignments, unresolved identities and conflicts before staging
global and application drafts together. Export writes only the selected scheme
artifact. Nondefault contexts use an explicit extension that KDE ignores.

**Custom Keyboard Shortcuts** stages installed application registration and
custom command creation, editing and removal. Apply verifies native registration
and saved definitions; applications and commands are never launched during
discovery or configuration. Pending drafts and jobs survive navigation, and
closing checks all managed dirty settings pages.

Import source, mappings and checked rows survive page/dialog replacement. A
verified Custom Apply queues read-only global discovery when existing drafts
and jobs permit it; Resume scheme import returns to the retained binding review.
Discarding the review preserves settings drafts, and exit protects unfinished
imports. Application-component removal covers additional and empty contexts
without activating them. Execution adoption of edited commands remains
unverified and can require a service/session restart. See
[Batch 15 schemes](docs/kde-batch15-shortcut-schemes.md) and
[Batch 15 custom shortcuts](docs/kde-batch15-custom-shortcuts.md), extended by
[Batch 16 imports/removal](docs/kde-batch16-shortcut-imports.md).

## Native networking

**Network and Sharing Center** reads NetworkManager's adapter, connection and
IP information. Internet connectivity is the service's reported state, with an
explicit warning when checks are disabled or unavailable.

**Network Connections** provides native Windows 7 views for managed Ethernet
and Wi-Fi adapters, nearby wireless networks and saved connections. It supports
scanning, connection/disconnection, Open/WPA-Personal/WPA3-Personal credentials,
and saved connection name, autoconnect, IPv4/IPv6 addressing and DNS properties.
Unsupported profiles and devices remain read-only with an explanation.

Property edits are staged. Changes affecting an active connection use a timed
NetworkManager recovery checkpoint and require **Keep changes**; Revert or the
deadline requests restoration and verifies the result. The daemon owns recovery
even if the application exits. Inactive profile edits save without activating
the connection. Scans and pending operations survive page navigation.

Saved credentials remain with NetworkManager; the application does not retrieve
stored passwords. New passwords are passed directly through D-Bus. Detected
external edits stop the operation; an uncertain write disables further writes
for that daemon session. Recovery cannot provide cross-client atomicity when
external edits go undetected. VPN, proxy, hotspot, cellular, enterprise/hidden
Wi-Fi, routes, radio switches and profile deletion remain future work.
See [Batch 8 scope and validation](docs/kde-batch8-networking.md).

## Native personalization and fonts

The application uses only this project's custom Aero theme. Generic theme,
color-scheme, icon, cursor and application-style selectors have been replaced.
**Personalization** edits supported Aero glass color, transparency, reflections,
blur, pointer size and SMOD decoration options through native Windows 7 controls.
**Aero Components** shows the approved identities and detected drift.

**Restore Aero defaults** stages the approved component identities, Aero palette
and supported tuning defaults. Review the pending changes and click Apply to
save. Wallpaper, fonts, desktop layout and unrelated preferences are preserved.
Missing or changed approved assets, unsupported runtime and locked configuration
prevent Apply. Saved values and runtime reload requests are verified separately
from rendered appearance; some applications and pointers need restart/sign in.

The versioned contract binds component names to the verified installed assets.
Changes to our custom assets require a deliberate contract update. Legacy
appearance writers and unrestricted appearance launchers are disabled. This
policy controls this application; external settings tools remain independent.
See [Batch 7 scope and evidence](docs/kde-batch7-aero.md).

**Desktop Background** selects a local picture and Fill/Fit/Stretch/Center for
one existing Plasma desktop. Other screens and activities retain their settings.
Apply checks the selected desktop again inside Plasma's scripting call and reads
back its picture and position. This confirms Plasma's preference, not disk sync
completion or a rendered-pixel comparison. Inactive desktops and policy-locked
or unwritable configuration stay unavailable. Slideshows and other wallpaper
plugins can be replaced explicitly with a single picture; their own editors are
not yet implemented.


**Font Settings** stages six KDE font roles and local fontconfig antialiasing,
hinting and subpixel overrides. Both configuration files are read back, while
unrelated and conditional fontconfig rules are preserved. Changes affecting both
files cannot be atomic; partial saves are reported explicitly. Apply currently
requires KDE Wayland; X11 resource export remains unsupported. Restart
applications for changed rendering preferences.

**Fonts** has asynchronous inventory, search, keyboard navigation and native
editable previews. Install copies a validated font into the current user's
`fonts/controlpanel` data directory without replacing files. Delete is available
only for unchanged files installed and recorded by this application; system
fonts and other user fonts remain protected. Cache refresh and a fresh inventory
run after changes, retaining failures and unverified results. Jobs survive page
navigation and remain visible in the work strip and Details. Font enable/disable,
duplicates, system-wide management and a full font-face browser remain unfinished.

See [Batch 6 scope and validation](docs/kde-batch6-personalization.md).

## Native KDE integration proofs

**User Accounts** now edits the current local user's display name through
AccountsService. It checks identity/name before and after explicit authorization,
supports cancellation before dispatch, and distinguishes denied, dismissed,
failed and uncertain outcomes. Once a change is sent, it waits for readback;
timeout/disconnection disables further writes in that session because the
request might still finish. Passwords, other users, account types and avatars
remain outside the native editor; catalog launchers retain development bridges.

**Aero Effects** opens the shared native Personalization editor. Its extension
controls require the recorded plugin build, KWin 6.6.6 Wayland and an already
loaded effect. The retired blur-only production writer cannot bypass the
versioned Aero contract. KWin exposes no rendered-strength readback, so the UI
reports saved preferences and reload outcomes without claiming visual proof.

**Ubuntu Package Preview** simulates an APT full upgrade using cached indexes.
It shows proposed changes and diagnostics with cancellation and retained
background state. This page performs no package changes, downloads or index
refresh. The audited APT 3.2.0 provider disables transaction hooks and custom
resolvers, bounds output/time and validates the complete plan; unsupported
versions fail closed. Existing package installation/update pages still require
their documented providers. See [Batch 5 evidence](docs/kde-batch5-backend-proofs.md).

**Screen Resolution** stages advertised fixed modes, enablement, arrangement,
fractional scaling and orientation across connected outputs. A complete draft
survives switching monitors and leaving the page. **Apply** starts the independent,
unprivileged `controlpanel-display-guard`; **Keep changes** confirms a verified
preview. Revert, the 15-second deadline or loss of the application restores owned
changes when fresh identity, topology and transaction checks permit it. Conflicting
external changes stop stale rollback. A hung UI cannot block the watchdog's output;
a stalled KScreen command can delay rollback completion. Mirroring, automatic
preferred-mode editing, HDR/VRR/color and additional controls remain open.

**Power Options** stages AC, battery and low-battery display/idle actions,
dimming, display and keyboard brightness, locked-screen display policy,
power-button/lid actions, sleep mode and global low/critical battery behavior.
Each control uses audited native choices, hardware capabilities and configuration
policy. Unknown current defaults remain marked while supported explicit values
can be saved independently. Raw unsupported values survive unrelated edits.
Apply requires saved-value readback plus the pinned PowerDevil owner's reload
reply and signal. Saved-but-unacknowledged outcomes retain their pending draft;
applications and activities may still inhibit idle actions. Performance-profile
radio buttons remain separate immediate actions. Custom timeouts use KDE's native
ranges through one week. **Restore defaults** reviews one source, all sources,
battery policy or all supported power policies before staging. It preserves native
inherited/global reset semantics and unchanged explicit overrides. Custom scale
entry previews native rounding; untouched raw scales and signed display positions
are retained without clamping. See [Batch 17](docs/kde-batch17-display-power.md)
and [Batch 18 scope and evidence](docs/kde-batch18-defaults-custom-values.md).
Additional power/display controls and real effects/persistence remain open.

**Window Behavior** edits the KWin title-bar double-click action: maximize/restore,
minimize, or do nothing. Existing other KDE actions remain visible until explicitly
replaced. Apply writes one KConfig key, sends KWin's no-reply reconfigure request,
and verifies both the saved value and the active option through KWin diagnostics.
External edits preserve staged choices and require reconciliation through Revert.
A saved file with failed runtime verification is reported as a failure, with its
persisted value shown separately.

These are scoped integration proofs for the current installation. The PowerDevil
schema and KWin diagnostic format are gated to verified version **6.6.6**;
unsupported versions disable editing. Display parsing targets the installed
libkscreen **6.6.4** JSON schema. See
[the integration evidence and remaining work](docs/kde-batch4-integration.md).

## Navigation, search, and keyboard access

The home screen offers **Category**, **Large icons**, and **Small icons** through
**View by**. Icon views show the same alphabetized applets; the selected view is
remembered as an application preference. Category links, search results, icon
items, and sidebar destinations share a typed action catalog and capability
checks. Unavailable actions stay disabled with an explanation. Existing KDE
launchers are development bridges and do not count as native settings coverage.

Search matches all entered words across titles, categories, descriptions, and
Windows/KDE aliases, ignoring case and accents. For example, **Windows Firewall**,
**Chrony**, **NTP**, and **PowerDevil** find their corresponding settings. Results
include unavailable features with their reasons. Editing a query stays within
one history step; Back/Forward restore queries, and clearing search returns home.

Use **Ctrl+F** to focus search, **Alt+Left/Right** for history, and **Alt+Home** for
Control Panel Home. **Escape** clears search. **Tab/Shift+Tab** move between enabled
controls; task links and applet items accept **Enter** or **Space**. Lists use
arrow keys, and focus has a visible dotted outline. Search keeps typing focus
while updating results; opening a destination transfers focus into that page.
Shared links expose accessible names, descriptions, and activation actions.

For new shell actions, add an explicit stable `ActionId` and its definition in
`src/ui/ActionCatalog.cpp`, then reference its ID from category data. Keep
capability discovery read-only, pass external commands as fixed argument lists,
and give unsupported actions an explanation instead of a working-looking link.

## Tests

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Tests use temporary configuration files, fake operation runners, and offscreen
UI fixtures. They do not change the system clock, NTP services, firewall, audio
settings, display modes, power policies, window preferences, real user fonts,
appearance preferences, network connections, or installed packages. Qt6 Test is required for tests; use
`-DBUILD_TESTING=OFF` for an application-only build.
Lifecycle tests use delayed fake jobs and injected settings backends to cover
page destruction/reentry, Apply/readback failures, external-change conflicts,
navigation decisions, and the application exit guard. Benchmarks are simulated
in these tests rather than run against the working machine. Catalog and shell
tests cover action integrity, aliases, unavailable launchers, icon layout,
keyboard/accessibility behavior, search history, and unsaved-change protection.
Optional offscreen Windows-style screenshots can be captured with
`QT_QPA_PLATFORM=offscreen LINUX_CONTROL_UI_CAPTURE_DIR=/tmp/control-ui build/shell_navigation_test captureWindowsStyleIfRequested`.
This fixture uses an isolated configuration directory and unavailable external
launchers; gray KDE items in its captures demonstrate capability gating.
`kde_integration_test captureWindowsStyleIfRequested` accepts the same capture
environment variables for the new pages and display confirmation state.
Display guard tests use a separate test-only binary and fake doctor to exercise
Keep, timeout, EOF, and blocked output; that test binary is never installed.
Power reload tests use a private D-Bus daemon with fake services.
Audio tests use a private fake pactl and injected sessions, with no live audio
playback, recording or writes. `audio_dialog_test` supports optional AeroQt
captures. `build/audio_backend_probe` performs manual read-only discovery.
Networking tests use a private fake NetworkManager for protocol, authorization,
recovery, concurrent edits and delayed replies; they never use the system bus.
`network_pages_test` accepts the screenshot environment variables above.

`build/network_backend_probe` is a manual **read-only** NetworkManager check.
It reads existing adapters, profiles, IP configuration and permissions; it does
not scan, retrieve secrets, create checkpoints or change connections.

`build/kde_backend_probe` is a manual **read-only** check of the installed KDE
providers. It reads capability/configuration data and never selects a value,
requests a reload, or calls Apply. Run it from the graphical KDE session; it is
not part of CTest's isolated fixtures.
By default it does not start a package preview. Its explicit
`--simulate-packages` option runs the hardened, read-only APT simulation too.

## Features

- Windows 7-style Control Panel home screen, organized by category
- Windows Update page backed by real `pacman` update checks
- Installed Updates history from `pacman.log`
- Programs and Features: list, uninstall, and reinstall (AUR-aware) installed packages
- Native Network and Sharing Center and Ethernet/Wi-Fi connection properties
- Native audio device levels, profiles, ports, channel balance and application routing
- Firewall status and rule summary (reads `ufw` configuration)
- Power Options
- Performance page with a Windows Experience Index-style benchmark

## Part of the WSL (Windows-alike Software for Linux) series

Why don't you also check out the other ones?

- [Linux Device Manager](https://github.com/actuallyaridan/linux-devmgmt)
- Linux Control Panel

## Native screen locking

The Aero Screen Locking page edits KDE idle timeout, resume locking and password
policy with staged Apply/Revert and native defaults. Its shortcut link reuses the
existing global shortcut editor. The fixed Aero background is currently passive
file evidence on this policy page; the linked Aero Login page provides its
separate reviewed appearance workflow.
See [Batch 21 coverage and validation](docs/kde-batch21-screen-locking.md).

Screen Locking file reads and saves run in workers. Slow saves remain tracked
until completion, including after view or session destruction; Close waits for
them, and the draft stays busy. Font and Keyboard failed synchronization also
cleans up unsaved KConfig dirtiness before reporting failure. Their native saves
and several other providers still need migration off the GUI thread. See
[Batch 23 lifecycle scope](docs/kde-batch23-save-lifecycle.md).

## Native Aero login appearance

Aero Login covers startup animation, startup sound, user selection and the shared
login/lock picture, with staged Apply/Revert and Restore Approved Defaults. The
theme remains fixed to Aero. A new narrow administrative helper uses guarded
file transactions and explicit recovery; the current editor remains read-only
until its trusted system helper is installed. Host discovery verifies the Aero
assets as root-owned. Real greeter rendering,
authorized writes and sign-in persistence remain unverified. See
[Batch 22 scope and validation](docs/kde-batch22-aero-login.md).
