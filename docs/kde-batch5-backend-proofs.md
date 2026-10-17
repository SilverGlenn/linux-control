# Batch 5: remaining backend feasibility proofs

Batch 5 follows the native Personalization/Fonts expansion, per the corrected
work order. It adds a representative authorized AccountsService operation,
native configuration of an installed nested Aero effect, and a real read-only
Ubuntu package transaction preview. These are scoped proofs for the recorded
installation, not complete Accounts, Effects or Ubuntu package management.

## Current-user account display name

The native User Accounts editor changes the current local non-system user's
display name through `org.freedesktop.Accounts.User.SetRealName`. It leaves login
name, UID, account type, password and other users outside its write scope.
Identity is anchored to `getuid`/`getpwuid`, the service's unique D-Bus owner and
the returned account object. Fresh preflight checks run before authorization and
again before dispatch. Completion requires matching identity and name readback;
a successful method return alone is insufficient.

The recorded packages are accountsservice 23.13.9-8ubuntu5.2 and polkitd
127-2ubuntu1.1. Their installed interfaces and policy are the primary contract:

- `/usr/share/dbus-1/interfaces/org.freedesktop.Accounts.xml`
- `/usr/share/dbus-1/interfaces/org.freedesktop.Accounts.User.xml`
- `/usr/share/polkit-1/actions/org.freedesktop.accounts.policy`

The backend explicitly checks the
`org.freedesktop.accounts.change-own-user-data` polkit action using its own
system-bus-name subject and interactive authorization. The
[polkit Authority interface](https://polkit.pages.freedesktop.org/polkit/eggdbus-interface-org.freedesktop.PolicyKit1.Authority.html)
defines the check, cancellation ID and dismissed-dialog result. An app Cancel
invalidates the token and prevents dispatch, even if authorization later returns
success. Cancellation is offered only before `SetRealName` is sent. Once sent,
the editor waits for bounded completion/readback. A timeout/disconnect leaves
uncertain state and disables further writes in that persistent session to avoid
a late request racing a retry. It does not claim server-side undo.

Explicit polkit denial, dialog dismissal, cancellation, failure and uncertain
outcomes are distinct. AccountsService's own permission error does not reliably
distinguish a denied secondary prompt from a dismissed one; the UI says so.
Other account task links remain visibly unavailable in this native editor.

## Installed Aero Glass Blur preference

Aero Effects stages one installed nested preference:
`kwinrc [Effect-aeroglassblur] BlurStrength`, default 3 and native range 2–15.
Apply preserves unrelated settings and effect enablement, checks the saved value
immediately before writing, then calls only
`/Effects org.kde.kwin.Effects.reconfigureEffect("aeroglassblur")`.

Compatibility requires KWin 6.6.6 on Wayland, an already loaded effect and the
exact installed plugin identity. The recorded plugin SHA256 is
`4ed00683b05ee02593c1b99da4f1aa864676ece792e7f56ab92cf83f1fd34d4e`.
Metadata is inspected without loading plugin code. Unknown builds or unavailable
effects disable Apply rather than guessing their schema.

The local AeroThemePlasma checkout includes
`repos/aeroshell-kwin-components` at commit
`97cc934b54aa9b237a8b814cb86f93f7d33b5ebf`. The
`effects_cpp/wayland/kde-effects-aeroglassblur` sources establish:

- `src/blur.kcfg`: group, key and default.
- `src/kcm/blur_config.ui`: range 2–15.
- `src/kcm/blur_config.cpp`: `kwinrc` persistence and effect reconfiguration.
- `src/blur.cpp`: actual strength consumption and private preview behavior.

The installed plugin matches that source build's hash. The backend requires a
reconfigure method reply, a loaded-effect recheck and saved-value readback.
KWin exposes no live blur-strength property for this plugin; the UI explicitly
does not claim to verify rendered strength. Another native color preview can
temporarily override effect behavior. No shared-memory preview is used here.

## Ubuntu package preview

PackageKit 1.3.4's installed apt backend advertises GetUpdates and UpdatePackages,
including simulation of a selected update set. It does not advertise
UpgradeSystem. That broader API is not a guaranteed equivalent of APT's
`dist-upgrade`; PackageKit is not treated as generally unavailable.

The native Ubuntu Package Preview uses APT 3.2.0's built-in full-upgrade resolver
with current cached indexes. It displays proposed upgrades, dependencies,
removals, configurations, deferred count and diagnostics. It has no apply action.
No install, removal, download or index refresh is dispatched.

Source review of [APT 3.2.0 private-install.cc](https://salsa.debian.org/apt-team/apt/-/blob/3.2.0/apt-private/private-install.cc)
found that some configured hooks run even when simulation is requested. The
backend therefore prepares a private temporary configuration that clears the
install, DPkg, AptCli and update hooks, plus Version/Binary overrides that could
restore hooks. The immutable argv pins simulation, no download, no locking,
legacy output, built-in solver/planner and no binary cache writes. Inherited
`APT_CONFIG` is removed; locale is fixed to C. An initial version check refuses
unaudited APT versions before running the resolver. These choices deliberately
exclude custom package hooks/resolvers from the preview.

APT's initial `--version` phase exits while parsing the command line, before
transaction dispatch or hooks. The private configuration is required for the
resolver phase; both phases remove inherited `APT_CONFIG`.

The [PackageKit 1.3.4 apt backend](https://github.com/PackageKit/PackageKit/tree/v1.3.4/backends/apt)
source establishes its advertised roles and simulation path. The APT fallback
provides the requested full-upgrade scope without inferring different PackageKit
semantics.

The service owns its process and retained result independently of the page.
Navigation continues while it runs; window exit waits for it. Cancellation is
safe for this read-only subprocess and remains pending until the process stops.
Timeout, truncated output, malformed records, inconsistent numeric totals,
metadata errors and nonzero exits never become a successful preview. APT does
not lock package state during simulation; concurrent package changes and stale
indexes can make the result obsolete.

## Windows 7 presentation and validation

All new controls use native Windows-style pages. Search/catalog/sidebar routes
find the account display name, Aero effect and Ubuntu package preview.
Application-style changes can trigger KDE to replace the editor's own QStyle;
an application-local guard restores AeroQt after that event. It does not undo
the desktop style preference. Native font dialogs/previews and command bars
retain readable light controls under a dark desktop theme.

Tests use fake account/polkit services on a private D-Bus, injected effect
runtimes and temporary KConfig, and fake APT executables. They cover denied and
dismissed authorization, cancellation/late replies, identity and name conflicts,
timeouts, readback failures, compatibility locks, exact simulation arguments,
disabled hooks, parser failures and retained navigation/exit behavior.

The hardened live simulation reported 5 upgrades, 0 new packages, 0 removals and
4 not upgraded, using current cached indexes. Before/after SHA256 hashes of
`/var/lib/dpkg/status` and `/var/lib/apt/extended_states`, plus names/sizes/mtimes
of all 145 index files, were unchanged. No real account change, effect reload,
desktop preference write or package mutation was performed for validation.
Final verification succeeded: the application builds and all 42 CTest suites
pass, including private account/polkit and PowerDevil bus fixtures. The real
read-only probe reports the current account editable, the pinned Aero effect
loaded with saved strength 3, font and component capabilities available, and no
provider errors. The compiled service's explicit `--simulate-packages` probe
also completed with 5/0/0/4 and unchanged package state/all 145 index records.
Native screenshots were inspected under AeroQt; final account and package
controls remain readable under the installed dark Qt base theme. Installation
was staged only under `/tmp/linux-control-batches5-6-stage.wz2mm9lk`; no real
installation or preference mutation was performed.

Final verification also fixed a pre-existing deferred search-refresh race that
could erase newly typed text after navigation. A deterministic committed-input
regression and ten consecutive shell-navigation runs passed.
