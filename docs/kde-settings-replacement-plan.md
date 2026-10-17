# KDE System Settings replacement audit and plan

Turn linux-control into the everyday replacement for KDE System Settings on the current Ubuntu KDE installation, with Windows 7 presentation throughout. Preserve the existing AeroQt shell and useful system integrations, introduce reliable settings backends, and close the coverage gaps against a recorded KDE baseline.

The application is a useful visual and integration foundation. It is not yet a complete settings frontend: many settings still open external KDE dialogs, unsupported controls are disabled, and native coverage remains limited to the explicitly implemented capabilities.

The audit below records the state before Batch 1. Batch 1 adds safe Chrony/timesyncd server handling, truthful unavailable/unknown statuses, and visibly disabled placeholders. Batch 2 introduces persistent staged color-scheme settings with Apply/Revert, external-change conflicts, and navigation/exit guards. Application-owned services now retain package checks/transactions, inventory reads, power writes, and benchmark progress across page replacement; shared read loaders cover devices and update history. See the current implementation limits and test instructions in [README](../README.md). The captured module inventory remains the original comparison baseline. Batch 3 adds a catalog of 134 stable actions, shared capability checks, live search with Windows/KDE aliases, working category/large/small icon views, keyboard navigation, and accessible task links. Native applets and existing development launchers are discoverable without enabling unsupported sound schemes or additional clocks. Search and view navigation retain Batch 2 guards and history behavior. Batch 4 implements native screen-mode selection with an independent rollback watchdog, PowerDevil display/sleep policy, and a KConfig-backed KWin title-bar preference with runtime verification. The catalog now has 136 actions. These proofs share the staged lifecycle and global work/exit protection. Their version constraints and validation are recorded in [Batch 4 integration evidence](kde-batch4-integration.md). Authorized network/account operations, nested Aero configuration, APT/PackageKit simulation, and broader native backend coverage remain outstanding milestone 1 work.

Batches 11 and 12 add native Keyboard and Default Programs pages with shared
staged lifecycle, source-grounded provider mappings and bounded readback.
Keyboard covers repeat/layout/options and OSD preferences; shortcuts and preview
remain gaps. Default Programs covers twelve roles and compatible MIME handlers;
full association editing and real application/cache adoption remain gaps.
The catalog now has 149 actions. See [Batch 11](kde-batch11-keyboard.md) and
[Batch 12](kde-batch12-default-apps.md) for exact scope and evidence limits.
These additions do not establish complete-module parity.

## Product decisions and target

Batch 6 expands native Personalization with per-desktop backgrounds and
independent icon/cursor/application styles, adds KDE font/rendering settings,
and provides searchable native font preview plus scoped current-user font
installation/removal. The catalog has 138 actions. These are partial Milestone 2
capabilities, not a claim of complete appearance parity. See
[Batch 6 scope and evidence](kde-batch6-personalization.md). Batch 5 follows it
with native current-user display-name editing, the installed Aero Glass Blur
strength, and a hardened APT full-upgrade simulation. The catalog has 141
actions after Batch 5. [Batch 5 evidence](kde-batch5-backend-proofs.md) records authorization,
cancellation, compatibility limits and simulation safety. Full account/network
management, comprehensive nested Aero settings, package mutations and the other
parity work remain on the backlog; these scoped proofs do not close every
milestone acceptance case.

The agreed first target is the current installation. The finished application must use its own Windows 7 controls for every supported settings surface. Embedded KDE configuration modules are permitted during development, but do not satisfy the final UI requirement.

### Aero-only release requirement (17 October 2026)

The product's theme is the custom Aero theme being developed for this project.
The finished application must not offer any other KDE global theme or let users
replace its coordinated Plasma style, application style, window decoration,
color scheme, icons or cursors with unrelated components. Alternative-theme
selection is intentionally outside the release scope, rather than a parity gap.
This decision supersedes the earlier proposal to add general theme, decoration
and global-theme preset pickers.

The current development installation reports this starting contract, read
without changing settings:

| Component | Current configured identity |
| --- | --- |
| KDE global theme | `authui7` |
| Plasma desktop style | `Seven-Black` |
| Qt application style | `kvantum` / `Windows7Aero` |
| Color scheme | `Aero` |
| Icon theme | `Windows 7 Aero` |
| Cursor theme | `aero-drop` |
| Window decoration | `org.smod.smod` / `SMOD`, resource `Aero` |

Batch 7 binds these identities to a versioned contract tied to the project's assets
and supported backends. They are a recorded starting point, not proof that an
arbitrary installed package with the same name is approved. Deliberate changes
to the custom Aero assets update that contract during development.

Personalization will expose supported customization within the Aero design:
wallpaper, font preferences, Aero colors/accents, blur, pointer size and installed
SMOD options where their actual behavior is supported and verified. It will not
provide arbitrary theme import, installation, replacement or generic presets.
Batch 7 replaces the Batch 6 generic selectors with native Aero customization
and component status. Retired production writers cannot bypass the new policy.

The restriction belongs in both the UI/action catalog and write backends.
Search, saved navigation, presets and direct backend calls must not bypass the
approved component identities. External changes should produce truthful drift
or conflict state and an explicit Aero repair workflow, without silently
overwriting unrelated user settings. This is an application/release requirement;
preventing independent tools or an administrator from changing KDE requires a
separately specified session or operating-system policy.

| Baseline component | Observed version or state |
| --- | --- |
| Distribution | Ubuntu 26.04.1 LTS |
| Desktop session | KDE on Wayland |
| Plasma workspace and KWin packages | 6.6.6 |
| System Settings package | 6.6.5 |
| KDE Frameworks Config and KCMUtils | 6.24.0 |
| Qt Core | 6.10.2 |
| Network and audio packages | NetworkManager 1.54.3; PipeWire Pulse server 1.6.2; WirePlumber 0.5.13 |
| Package management | APT present; PackageKit installed; pacman and checkupdates absent from PATH |
| Time synchronization | Chrony installed; systemd-timesyncd not installed |
| Source baseline | Commit `280a1bd`, plus the existing local CMake dependency and RPATH edits |
| Audit date | 16 October 2026 |

Installed packages establish available implementation candidates, not that every service is running or every optional hardware capability exists. Backend service discovery must determine that at runtime.

The [baseline inventory](audits/kde-settings-baseline.json) records all 105 modules returned by `kcmshell6 --list`: 83 in the System Settings plugin namespaces, 20 in Info Center, and two top-level information modules. It also records the separately installed KRunner settings module and 26 compiled configuration plugins for decorations, effects, and KRunner providers. Those counts describe modules and plugins, not unique settings or percentage completion. Script and QML extensions can add configuration surfaces beyond those compiled plugins.

The release scope is feature parity with the applicable settings on this recorded installation within the Aero-only product policy, including its installed AeroShell and Aero theme configuration. Alternative mobile frontends can map to the same capabilities instead of producing duplicate applets. Information pages should be tracked separately from writable settings. New plugins or Plasma versions require an updated inventory and explicit compatibility work; discovery alone cannot convert arbitrary plugin interfaces into Windows 7 controls. Inventory entries for alternative theme selection remain historical evidence and are classified as excluded by product policy for release.

## Audit status

The maintained [capability backlog](audits/kde-capability-backlog.json) and
[parity status](kde-parity-status.md) expand the frozen baseline into installed
schema entries, provider controls and unresolved coverage obligations. Their
separate acceptance dimensions prevent source/fixture coverage from being
mistaken for verified live parity. The original findings below are historical.

The following findings are based on source inspection and read-only package/module discovery. No real setting changes, privileged operations, package transactions, or destructive device actions were exercised. The existing incremental build passes with `ninja: no work to do`; a clean build and end-to-end runtime verification remain release work.

The audit labels below distinguish real implementation paths from complete coverage. A read or write path in source does not establish that every error case works on the target machine.

### Existing functionality

| Area | Implemented behavior | Work needed for replacement |
| --- | --- | --- |
| Shell and navigation | Eight Windows 7 categories, breadcrumbs, back/forward, category pages, native applets, and AeroQt styling. | Implement search and icon views, unify action routing, preserve settings state, add keyboard and accessible control semantics. |
| Personalization | Discovers KDE color schemes, reads the selected scheme, launches the real Plasma scheme application tool. | Global themes, Plasma/application styles, wallpaper, decorations, icons, cursors, and detailed color editing are absent or external. Scheme selection is marked successful before the helper result is checked. |
| Fonts | Enumerates installed families using fontconfig; launches a font previewer. KDE font-setting links work through an external module. | Native font selection/rendering controls, install/remove/enable/disable, duplicate handling, and management of user versus system fonts. The Organize bar is decorative. |
| Sound | Lists Pulse-compatible sinks/sources, changes defaults, and captures level meters. Properties shows parsed information. | Volume/mute controls, device ports/profiles, per-application routing, complete properties/configuration, actual notification sounds, and correct Apply/Cancel behavior. Configure and Apply lack functional handlers. |
| Date and time | Reads system time/timezone; changes time, timezone, and NTP through timedatectl. | Replace unsafe server configuration, support the target's time provider, handle permission and process failures, and serialize changes. Additional-clock and clock-notification preferences are app-local. |
| Power | Reads and changes advertised power-profiles-daemon profiles and follows profile changes. | Implement PowerDevil idle/sleep, display dimming/off, brightness, lid/power-button policies, battery thresholds where supported, and locking integration. A PPD profile is not a complete power plan. |
| Network | Shows a simplified hostname/default IPv4 route overview. | Real connection state, IPv6, Wi-Fi, Ethernet, VPN, DNS, routes, proxy, sharing, and connection editing. Editing currently opens KDE's connection module. A default route does not establish Internet reachability. |
| Firewall | Reads ufw configuration and counts configured rules. | Native enable/disable, policy/logging controls, rule CRUD, privileged authorization, live state, and clear unknown/unavailable states. All configuration sidebar actions are inert. |
| Accounts | Reads current identity, some group membership, and avatar candidates. | Native user enumeration and account creation/deletion/password/avatar/account-type actions. Existing edit links open KDE; the UAC link also opens Users, which is not equivalent policy configuration. |
| Accessibility | Windows-style overview and links to KDE modules. | Native accessibility settings and actual actions for magnifier, screen reader, virtual keyboard, contrast, and input assistance. Several Start labels currently open configuration rather than starting the named tool. |
| Devices and printers | Substantial sysfs device enumeration, CUPS listing, read-only properties, and external pairing/setup launchers. | Native Bluetooth pairing/trust/removal, printer add/remove/default/options/jobs, removable storage policies, device actions, and conditional Thunderbolt/camera support. `DeviceOps.h` contains declarations without implementations in this repository. |
| Programs and Features | Explicit-package inventory and real uninstall/reinstall paths on pacman/yay systems. | APT/PackageKit implementation for this target, transaction previews and dependency changes, robust errors, persistent operation ownership, and a clear distinction between applications and packages. |
| Linux Update and history | Real pacman/checkupdates/yay paths, progress parsing, and pacman-log history. | Ubuntu update integration, update behavior preferences, reboot/offline-update handling, history, package locks, and honest check failures. Current Arch commands cannot provide the feature on this installation. |
| System | Reads OS and hardware information; supports optional Windows branding. | Useful system-setting destinations and truthful identity/details regardless of cosmetic branding. Several advanced/remote/protection links are inert. |
| Performance | Custom CPU/memory/disk/OpenGL benchmark with stored scores. | Keep as an additional tool; it is not KDE settings parity and its scores are not Windows WEI scores. Most adjustment sidebar links are inert. |
| Action Center | Reads ufw enabled text, checks for clamscan, and looks for a polkit service. | Replace canned health assertions with provider-backed facts or explicit unmonitored states. Binary presence does not establish active antivirus protection. |

Key source locations: [MainWindow.cpp](../src/ui/MainWindow.cpp), [Commands.h](../src/ui/Commands.h), [Win7Ui.h](../src/ui/Win7Ui.h), [page implementations](../src/ui/pages), [dialog implementations](../src/ui/dialogs), and [SysfsScanner.cpp](../src/devices/SysfsScanner.cpp).

### Defects to address before expanding coverage

| Priority | Finding and evidence | Required correction |
| --- | --- | --- |
| P0 | Editable NTP server text is interpolated into a script passed to `pkexec /bin/sh -c` in `DateTimeDialog.cpp:776-785`. Shell syntax can become unintended privileged commands after authentication. | Remove interpolation into elevated shell code. Validate a typed server value in a narrow privileged helper, use atomic configuration writes, and test rejection of malformed input. |
| P0 | Internet Time restarts and writes configuration for timesyncd, while this installation has Chrony and no timesyncd package. NTP toggle and server configuration also start independent privileged processes. | Detect the provider; implement Chrony through a reviewed backend, or show unavailable server editing until it exists. Use one ordered operation with explicit failure handling. |
| P0 | Action Center claims automatic updates, recommended Internet security, and active maintenance without probing them (`ActionCenterPage.cpp:284-333`). Firewall file-read failures become Off/default policies. | Distinguish known enabled, known disabled, unavailable, unknown, and failed checks. Remove unsupported assurance text. |
| P0 | Update checking can report no updates after a failed database sync, and checkupdates errors are not reliably distinguished from empty results (`LinuxUpdatePage.cpp:1044-1052,1102-1115`). | Preserve failure separately from up-to-date state; show provider and last successful check. Disable unsupported package operations on Ubuntu until a backend exists. |
| P1 | The Arch fallback refreshes the shared database with `pacman -Sy`; selected updates use `-Sy` with only some packages (`LinuxUpdatePage.cpp:1031-1069,1166-1176`). | Rework the Arch adapter around a reviewed full-upgrade policy. Do not carry these selective-upgrade semantics into the new transaction layer. |
| P1 | Sound defaults apply immediately, while Apply is unconnected and Cancel cannot restore the earlier default. Cosmetic scheme/startup/communications settings save only to app QSettings (`SoundDialog.cpp:183-188,420-433,495-663`). | Give each setting an explicit staged or immediate model. Implement real system behavior or visibly withhold the control; do not show saved app preferences as applied desktop policy. |
| P1 | Most navigation recreates and destroys the current page (`MainWindow.cpp:461-589`), while an update installation QProcess is parented to that page (`LinuxUpdatePage.cpp:1160`). | Move settings models and operation ownership outside views. Gate navigation on unsaved changes; package work must survive page changes and have an explicit exit policy. Exact process behavior on destruction needs a runtime test. |
| P1 | Category/task routing uses several text tables; home has only six explicit task routes (`MainWindow.cpp:709`), and unclaimed links can silently do nothing (`:963,:1118`). | Use typed action IDs from one catalog. Every advertised action must route, display a reason it is unavailable, or be removed. |
| P1 | Link controls and expanders rely on mouse events and often lack focus/key behavior (`CategoryWidget.cpp:72-92`, `LinkLabel.h:22-73`, `Win7Ui.h:317`). | Use real focusable Qt controls styled as Win7 links; provide accessible roles, names, focus indication, and Tab/Enter/Space activation. |

`Nav::plain` is not a reliable dead-link count: the update page's Check for updates entry is specially wired by MainWindow. The action catalog must replace this exception instead of relying on text searches to establish coverage.

## Architecture for the replacement

Keep the Qt Widgets and AeroQt presentation. Move Linux integration into domain services and make the page widgets views of those services. Each implemented setting must read and write the same authoritative state used by KDE.

### One settings catalog

Extend the existing PageId/PageRegistry work into a single catalog containing stable page and action IDs, Windows 7 category, translated title, keywords, source KDE module IDs, availability predicates, page factory, backend owner, and coverage status. Category tasks, sidebar actions, search, icon views, and command-line deep links must consume that catalog.

Use separate action IDs for operations such as checking updates or opening the microphone tab. Breadcrumb titles and branding remain display data. Unsupported Windows-era labels need honest Linux equivalents; HomeGroup and Windows UAC policy must not imply capabilities Linux does not provide.

### Settings models and page lifecycle

Each domain adapter exposes asynchronous loading, a typed snapshot, validation, application of changes, defaults, and observation of external changes. Pages keep staged edits separately from the live snapshot. A shared settings session tracks dirty state, busy operations, authorization, and explicit operation results.

Use Apply/OK/Cancel and Restore Defaults for staged applets. Defaults changes the staged values until Apply. If a control must act immediately, present that behavior explicitly and do not imply Cancel can undo it. On navigation or exit, ask to apply or discard pending edits. Re-read the backend after success before marking values applied; handle partial failure honestly instead of claiming universal rollback across unrelated services.

KDE's [configuration module lifecycle](https://api.kde.org/kabstractconfigmodule.html) provides useful reference semantics for loading, saving, defaults, dirty state, and authorization. It does not establish a universal typed setter for every KDE setting.

### Persistent operations and provider selection

An application-owned operation manager owns asynchronous package, device, and privileged jobs. Views subscribe to progress; leaving a page does not destroy the job. Cancellation and closing the application must respect the operation: cancelling a scan differs from interrupting a package database transaction.

Capability discovery checks actual services, module metadata, installed tools, session type, hardware support, and authorization. Missing and failed providers remain distinct from disabled settings. Select the provider by capability; do not assume that an Ubuntu host uses the same time, firewall, or package service as the upstream Arch installation.

### Reuse KDE implementation through explicit adapters

| Domain | Proposed integration | Important implementation boundary |
| --- | --- | --- |
| Configuration files | KF6 Config and appropriate generated schemas, plus the same apply/reload behavior as the target module. | Use KConfig defaults, immutability, and change notifications. A file write alone may not apply a running component's setting. |
| Appearance | Plasma's supported application tools and APIs, KConfig, and module-specific models where reusable. | Global themes affect several settings; preserve independent choices and test preview/revert. Do not treat a color scheme as an entire desktop theme. |
| Display | libkscreen and the target KWin/Wayland configuration flow. | Enumerate real output capabilities; use confirmation timeout and restoration after an unusable display change. Do not implement this using X11-only commands or direct output-JSON editing. |
| Input and window management | KWin input interfaces and configuration/reload paths appropriate to this Plasma version. | Separate global settings, per-device settings, session support, and nested effect/decoration configuration. |
| Network | NetworkManagerQt and NetworkManager settings/secrets interfaces; KDE's matching proxy configuration. | Include credential handling, VPN plugins, permissions, connection checkpoints where applicable, and IPv6. |
| Bluetooth | BluezQt/BlueZ with an agent for pairing interactions. | Authentication, trust, permissions, cancellation, and device disappearance need first-class behavior. |
| Audio | A typed asynchronous Pulse-compatible adapter for the installed PipeWire Pulse server, with WirePlumber policy support where required. | Defaults, ports/profiles, per-stream volume/routing, and communications policy are different operations. The latter needs an explicit capability investigation. |
| Power | PowerDevil configuration and live reload, UPower, and PPD only for the advertised performance profile subset. | Preserve AC/battery behavior and hardware-specific limits; do not create fictional Windows power plans. |
| Accounts and administration | AccountsService, the existing system services, and narrowly scoped KAuth/polkit helpers where needed. | Validate at both UI and privileged boundary; preserve authentication and never run the whole application as root. |
| Time | timedate1 for time/timezone and a detected time-provider adapter for server configuration. | Chrony is the first target; do not install or replace the user's time service as an incidental action. |
| Packages | Investigate the installed PackageKit provider for APT transactions, with Debian package metadata for inventory/history as needed. | Verify transaction simulation, dependencies, locks, progress, authentication, errors, offline updates and reboot state before choosing it. Keep Arch support behind a separate adapter. |
| Printers | CUPS APIs and the target's print-manager behavior. | Printer discovery, drivers/options, jobs, defaults, and authorization require more than enumeration. |
| Notifications, search, session and login | Existing KDE configuration schemas, service interfaces and module logic, wrapped by specific adapters. | Some module backends are private implementation details; version-pin reuse and verify it against the installed release. |

Public implementation references include [KConfig XT](https://develop.kde.org/docs/features/configuration/kconfig_xt/), [KConfigWatcher](https://api.kde.org/kconfigwatcher.html), [NetworkManagerQt](https://api.kde.org/networkmanager-qt-index.html), [libkscreen configuration operations](https://github.com/KDE/libkscreen/blob/master/src/setconfigoperation.h), and [PowerDevil](https://github.com/KDE/powerdevil). These support the adapter approach; individual controls still require inspection of the matching installed-version module implementation.

Do not design around automatically extracting every KCM backend and reusing it without its UI. Some modules couple their logic to QML or private classes. Reuse public libraries first; wrap or adapt specific module logic only after a feasibility check. KDE's [module loader](https://api.kde.org/kcmoduleloader.html) can host native modules during development, but it does not transform their controls into a Windows 7 interface.

Suggested code boundaries are `src/settings` for the catalog and settings sessions, `src/platform/kde` for KDE adapters, `src/platform/system` for system-service adapters, and `src/operations` for persistent jobs. Existing page classes can migrate incrementally rather than being rewritten at once.

## Coverage to implement

The inventory JSON maps every discovered top-level module to one of the groups below. This is a feature backlog, not a promise of one new page per module. Milestone 0 expands each applicable module into specific controls and subdialogs with evidence and acceptance cases.

| Windows 7 area | Settings surfaces to cover | Existing starting point | Milestone |
| --- | --- | --- | --- |
| Appearance and Personalization | Pinned Aero components, permitted colors/accents, wallpaper, pointer sizing, SMOD window-border options and Aero restoration. Alternative-theme selection is excluded by product policy. | Batch 7 native Aero customization, approved component status and staged restoration; native wallpaper/fonts. | 2 |
| Appearance and Personalization | Fonts, font rendering, DPI behavior, and font installation/management. | Family listing and external preview. | 2 |
| Appearance and Personalization | Window behavior, task switcher, rules, virtual desktops, activities, screen edges, animations, desktop effects, and KWin scripts. | No native editing. | 5 |
| Appearance and Personalization | Lock screen, session startup/logout, autostart, background services, splash screen, SDDM, Plymouth, and AeroShell SDDM settings. | Lock-screen launcher only. | 4 and 5 |
| Hardware and Sound | Display arrangement, resolution, scaling, refresh rate, rotation, supported color/HDR/VRR settings, Night Light, day/night scheduling, and applicable gamma controls. | External display module. | 3 |
| Hardware and Sound | Mouse, touchpad, keyboard layouts/options, shortcuts, touchscreens/gestures, tablets, game controllers, and virtual keyboards. | Batch 10 native Mouse/Touchpad; keyboard and advanced device editors remain open. | 3 and 5 |
| Hardware and Sound | Playback, recording, default devices, volume/mute, application routing, card profiles/ports, and audio CD preferences. | Batch 9 native device/application audio controls; advanced audio policy/events/tests remain open. | 3 and 5 |
| Hardware and Sound | PowerDevil policies, brightness, display/sleep timeouts, power/lid actions, AC/battery choices, and battery settings supported by hardware. | PPD performance-profile selector. | 3 |
| Hardware and Sound | Bluetooth, printers/jobs/options, removable storage and automount, device actions, Thunderbolt authorization, and cameras. | Device/CUPS enumeration and external wizards. | 4 and 5 |
| Network and Internet | Network connections, Wi-Fi/Ethernet/VPN, addresses/DNS/routes, IPv6, proxy, connection preferences, hotspot, and cellular settings where available. | Batch 8 native Ethernet/Wi-Fi, saved connections, IP/DNS properties and timed recovery. Advanced networking remains open. | 4 |
| Programs | Default applications, file associations, personal folder locations, and application behavior settings. | Some external launchers; many category headings are inert. | 5 |
| Programs | Installed applications/packages, uninstall/reinstall, update transactions, history, and update behavior. | Arch-specific code. | 6 |
| User Accounts | Account management, avatars/passwords/account type, online accounts, and KDE Wallet configuration. | Identity summary and external Users/Wallet launchers. | 4 and 5 |
| Clock Language and Region | Date/time/timezone, time provider, language, locale formats, and spelling. | Partial Date/Time dialog and external locale module. | 0 and 4 |
| Ease of Access | Accessibility options, input assistance, visual/audio assistance, screen reader and virtual keyboard integration. | External module launchers. | 4 |
| System and Security | Firewall control/rules, backups, update policy, supported health indicators, and real system facts. | Read-only ufw/system information; canned Action Center state. | 0, 4, 5 and 6 |
| System and Security | Search/indexing, KRunner providers, web shortcuts, notification behavior, event sounds, recent-file privacy, and feedback. | Sound placeholders; most surfaces absent. | 5 |
| Advanced applet tabs | Installed decoration/effect/provider options, including Smod, Aero glass blur and Aero glide. | No equivalent native controls. | 5 |

Keep Windows 7's eight categories and add suitable subpages or Advanced tabs for KDE-only capabilities. System Settings parity includes search, notifications, shortcuts and workspace behavior even though the original Windows 7 category catalog does not naturally expose them.

The System page can consolidate applicable information modules; it need not imitate Info Center's page layout. KDE's own landing page is replaced by the Win7 home, while any unique quick-setting functions must still have a destination.

## Delivery milestones

### Batch 7 Aero-only Personalization

1. Introduce the versioned Aero component contract and backend validation.
2. Replace or restrict generic theme/style/decoration/icon/cursor/scheme choices
   in release UI, search and routes. Development tooling must be explicitly
   separated from the release surface.
3. Add supported Aero-specific color/appearance controls, cursor sizing and
   native SMOD decoration options, keeping the approved component identities.
4. Add staged Restore Aero defaults/repair with preflight, readback and explicit
   partial-failure handling. Preserve unrelated settings and user customization
   outside the chosen reset scope.

Exit gate: release users cannot select an unapproved theme or component through
the application, including indirect action/backend paths. Supported Aero tuning
persists correctly; restoration and external drift have truthful outcomes.

Implementation and validation: [Batch 7 Aero-only Personalization](kde-batch7-aero.md).
The catalog now contains 142 stable actions, including Restore Aero defaults.

### Batch 8: native Network and Internet

Replace the everyday KDE network launchers with Windows 7 connection views and
property dialogs backed by the installed network service. Start with truthful
adapter/connection status, Wi-Fi scanning and connection, Ethernet, saved
connections, and automatic/manual IP and DNS settings. Keep operations owned by
the application, handle authorization and wrong credentials explicitly, and
protect changes that could disconnect the user with a verified recovery path.
Use the service's credential storage rather than general application settings.
VPN, proxy, hotspot and cellular support can follow as separate scoped work.
Implementation and validation: [Batch 8 native networking](kde-batch8-networking.md).
Everyday connection actions now use the native editor and shared session; the
catalog contains 145 stable actions. The backend uses NetworkManager's typed
system D-Bus API directly. The initial audit table above remains historical.

### Batches 9 and 10: native audio and pointing devices

Native Sound now covers endpoint and application volume/mute, defaults, routing,
ports, sound-card profiles and channel levels through an application-owned
session. Mouse and Touchpad provides staged, capability-gated KWin controls,
mode coupling and reported per-device defaults. Catalog actions and search use
these native destinations. The catalog contains 148 stable actions.

See [Batch 9 audio](kde-batch9-audio.md) and [Batch 10 input](kde-batch10-input.md)
for verification and explicit remaining scope. Keyboard/layouts/shortcuts,
advanced audio policy and real runtime acceptance remain on the full parity
backlog; these batches do not close their entire KDE modules.

### Batches 11 and 12: native keyboard and default programs

Keyboard now stages the installed provider's repeat, startup Num Lock,
model/layout/variant/label, policy/options and layout-notification preferences.
Wayland-specific differences and unobservable runtime effects are explicit.
Default Programs stages all twelve KDE roles and compatible individual MIME
handlers, with policy checks, external-edit conflicts and saved-file readback.
Inventory and file transactions run on a serialized worker. Both pages retain
pending work independently of their views and use the native Aero controls.

The catalog has 149 actions. [Batch 11](kde-batch11-keyboard.md) and
[Batch 12](kde-batch12-default-apps.md) record the precise subset. Shortcuts,
keyboard preview, complete association ordering/embedding and real runtime
acceptance remain open; no whole module is declared complete.

### Batch 13: native registered global shortcuts and compiled preview

The native shortcut editor discovers all registered components, contexts and
actions, preserves full four-chord alternatives, stages explicit reassignment,
and verifies native daemon and file persistence. Keyboard previews compile the
staged XKB model/layout/variant/options in-process with all symbol levels and a
native Aero display. PC 101/102/104/105 physical geometry is supported; other
models show an accurate key list with an explicit geometry limitation.

The catalog now has 150 actions. [Batch 13](kde-batch13-shortcuts.md) records the
implementation subset and evidence. Native standard application shortcuts,
schemes, custom command/application shortcut creation/removal, unregistered
per-layout registration/reload and real keyboard/inhibition acceptance remain
on the full goal. Native daemon readback does not prove action invocation.

### Batch 14: standard application shortcuts and per-layout activation

Application Shortcuts adds the installed KDE standard-action catalog in a
separate native Aero category tree and persistent session. It stages complete
alternatives, explicit reassignment and defaults, reads fresh layered native
configuration, preserves unrelated preferences and respects policy. Saved-file
readback and native change notifications are distinct from application adoption;
application-specific overrides and restart requirements remain explicit.

The global shortcut adapter maps exact translated names from KWin's runtime
layout list. On audited KWin 6.6.6, Apply can register an unassigned configured
layout binding without taking invocation ownership, persist it and wait for a
targeted layout reload acknowledgement. Compositor, runtime-layout and file
changes stop the operation; partial and uncertain outcomes preserve the draft.
Registration and reload acknowledgement do not prove physical key invocation.

The catalog now has 151 actions. [Standard shortcut scope](kde-batch14-standard-shortcuts.md)
and [layout activation scope](kde-batch14-layout-shortcuts.md) record the bounded
implementation. Schemes, command/application registration/removal, other model
geometry and live keyboard/application acceptance remain open.

### Batch 15: shortcut schemes and custom definitions

Global and Application Shortcuts review native `.kksrc` schemes, selected scopes,
components and actions, explicit command migrations and binding conflicts.
Selected binding imports validate both complete drafts before either changes.
Exports retain complete alternatives and use an explicit nondefault-context
extension. Built-in scheme discovery and file pickers use the native Aero UI.

Custom Keyboard Shortcuts adds installed-application registration and managed
command creation, editing and removal through a persistent staged session.
Native replies, full registry readback, saved definitions, catalog changes and
policy guard Apply. Close approval now checks every managed dirty session,
including off-page imports. The catalog has 152 actions.

Automatic definition-registration-refresh-binding coordination, multi-context
component removal and adoption of edited command definitions remain open.
Source/private fixtures do not establish physical invocation or live persistence.
See [scheme scope](kde-batch15-shortcut-schemes.md) and
[custom-shortcut scope](kde-batch15-custom-shortcuts.md).

### Batch 16: retained command-import coordination and whole-component removal

The app-owned import coordinator retains source, explicit mappings and checked
rows across views. Verified Custom Apply queues guarded read-only global
discovery; a fresh review precedes atomic binding drafts. Resume, suspend and
discard remain separate from Apply/Revert, and exit protects unfinished imports.
Existing unregistered command edits and registration share one staged batch.
Failed verification retains pending drafts even if requested values appear saved.

Audited context-qualified unregister and native pruning cover whole application
components with additional, empty or nondefault-only contexts. No context is
activated. Complete inventory, policy, owner, file and unaffected-state guards
protect the multi-step operation. Runtime Exec adoption of edited definitions,
arbitrary action deletion/activation, other keyboard geometry and full live
acceptance remain open. See [Batch 16 scope](kde-batch16-shortcut-imports.md).

### Milestone 0 Correctness and baseline

Fix the privileged NTP interpolation, detect Chrony versus timesyncd, and remove unverified health/success claims. Clearly disable unsupported package actions on Ubuntu. Give existing cosmetic and unavailable controls honest presentation. Preserve the local CMake edits.

Expand the recorded module list into a per-capability backlog: control ID, owning KDE module/version, Linux provider, expected reads/writes, dependent subdialogs, session/hardware conditions, and runtime acceptance cases. Include the recorded nested plugins and inspect QML/script-provided options. The result must separate optional hardware, duplicate frontends, information surfaces, and actual missing settings.

Exit gate: no known unsafe elevated-shell interpolation, unsupported package actions, silent fake success, or app-local preference masquerading as an applied system setting. Every observed module has an explicit scope classification and every applicable setting has a backlog entry.

### Milestone 1 Common settings infrastructure and feasibility

Introduce the typed catalog, settings session, provider capabilities, operation results, persistent operation manager, and navigation guard. Migrate one existing page before expanding the pattern. Implement real search and category/icon views using the catalog. Consolidate accessible task-link controls and translation-ready strings.

Run small feasibility implementations against this release for representative integration types: a KConfig appearance preference, a Wayland display operation with revert, a PowerDevil policy reload, an account or network operation with authorization, a nested Aero effect preference, and a PackageKit transaction simulation. Record any private-API dependency and its compatibility constraints. These resolve the highest architectural uncertainty before scheduling all applets.

Exit gate: applying or rejecting a staged change behaves correctly; navigation preserves state and jobs; errors and authorization cancellation stay distinct from success; external KDE edits refresh an unmodified page and produce a clear conflict when local edits exist. The representative integration paths have a reviewed implementation strategy.

### Milestone 2 Personalization and fonts

Complete native Windows-style appearance applets for the pinned custom Aero theme: supported colors/accents, wallpaper, Aero icon/cursor options, application styling and SMOD window-decoration configuration; then fonts/rendering and font management. Do not implement selection of alternative KDE themes or unrelated components. Use typed backends, enforce the release contract and confirm actual applied state. Integrate detailed installed decoration configuration under milestone 5 as supported.

Exit gate: the user can configure permitted Aero appearance and fonts without leaving the application; changes persist and agree with KDE after logout/login where required; cancellation and failed helpers leave truthful state; Aero restoration preserves unrelated choices and unapproved component replacements are rejected.

### Milestone 3 Display input audio and power

Implement native Display and Screen Resolution, Mouse, Keyboard, Touchpad and other applicable device applets. Complete Sound device controls. Extend Power Options to real PowerDevil policies instead of only PPD profile selection.

Exit gate: supported display changes have a confirmation timeout and restoration path; hotplug and device disappearance work; supported audio controls manipulate the real audio server; AC/battery and power actions match PowerDevil; unavailable hardware choices are explained. Shortcuts and advanced effect/input options can finish in milestone 5, but must remain on the parity backlog.

### Milestone 4 Network accounts regional accessibility and devices

Build native connection/property dialogs, Bluetooth and printer management, account administration, regional/language settings, provider-correct date/time settings, screen-lock policy and accessibility applets. Add native firewall rule/policy controls through a narrow backend.

Exit gate: representative connection, pairing, printing, account and locale tasks work end to end without external settings windows. Authentication cancellation, wrong credentials, failed connections, unsupported hardware and service loss have tested outcomes. Credentials are handled through their appropriate services rather than general application preferences.

### Milestone 5 Workspace advanced settings and local extensions

Complete shortcuts, window management, virtual desktops/activities, effects and scripts, startup/login/background services, notifications and sound events, search/indexing/KRunner, default applications/file associations, folder locations, online accounts, wallet, privacy, backups, removable storage and remaining device options.

Implement the current installation's nested configuration surfaces, including AeroShell SDDM, Smod and other installed decoration options, Aero glass blur/glide, other configurable effects, and KRunner provider preferences. Use Windows-style tabs, lists and dialogs for the final presentation. A schema-driven editor is acceptable only when the schema and explicit apply behavior provide equivalent controls; it cannot be assumed to cover arbitrary QML or custom plugins.

Exit gate: every applicable core and locally installed extension capability in the frozen baseline has native read/write coverage. Duplicate mobile frontends share those capabilities. Any remaining surface has an explicit documented limitation and blocks a full-parity claim unless it is genuinely inapplicable.

### Milestone 6 Ubuntu package workflows and retained tools

Implement the chosen APT transaction provider, installed-program/package inventory, transaction preview, dependency changes, update policy/history, lock/error handling, progress and reboot/offline-update state. Keep the existing Arch integration as a separate provider with corrected upgrade behavior. Preserve system/device information and the custom performance benchmark as extra tools.

This work can run alongside milestones 2 through 5 once milestone 1 establishes operation ownership and provider interfaces. It is necessary for the current application's Programs and Update pages, even though full package installation is broader than KDE System Settings itself.

Exit gate: Ubuntu package operations no longer invoke pacman or yay; unavailable providers never appear up to date; transactions survive navigation and disclose proposed removals and authentication requirements before execution. Tests use a disposable machine, not the working desktop's package database.

### Milestone 7 Parity verification packaging and release

Verify the per-setting backlog against a disposable copy of the baseline and applicable physical hardware. Finish localization, high-DPI layout, accessibility, keyboard use, startup performance, service dependencies and .deb packaging. Add stable command-line/deep-link destinations so desktop launchers can open individual applets.

Development-only embedded KCM access stays behind an explicit development option and cannot be reached through the normal release flow. Keep required KDE libraries, modules and services installed. Changing the desktop's default settings launcher is a deliberate release integration step after parity; this plan does not authorize changing it now.

Exit gate: the completion criteria below pass and a real desktop-use trial finds no task requiring the stock settings UI for the agreed scope.

## Completion criteria and verification

Track completion by capability, with separate columns for native UI, read, write, external-change refresh, defaults/reset behavior, error handling, and runtime verification. Do not count an external launcher, a saved app-only checkbox, or one control inside a large KCM as completed parity.

For each applicable setting, verify: open and read the current value; change and apply it; observe the live effect and persistence; reload it; confirm KDE sees the same state; test reset/defaults where applicable; cancel staged changes; and change the value externally while this program is open. Include unauthorized/denied writes, disconnected services/devices, invalid values and version mismatch. Settings that require logout/restart must say so instead of pretending to apply immediately.

Use focused automated tests for catalog/action coverage, settings-state transitions, provider errors and authorization results. Fake backends should cover failure paths without changing the real machine. Add integration tests in a disposable Wayland KDE session/VM, with hardware checks for display, audio, printers, Bluetooth and power where simulation cannot establish behavior.

Capture Windows 7 visual reference images for home, category, list, property tabs, dialogs and wizard pages. Verify at 100, 125, 150 and 200 percent scaling, long translated strings, keyboard focus, high contrast, and theme changes. Avoid fixed layouts that clip settings or prevent accessible navigation. The glass chrome, blue task links, typography, command bars, property tabs and dialog button conventions should remain consistent.

The final release must provide native Windows 7 routes for all applicable capabilities in the frozen baseline, with no normal-flow dependence on System Settings, kcmshell6, embedded native KDE settings screens, or external setup wizards. Services and supported helpers can remain implementation dependencies. Newly installed unknown extensions must be detected and reported as unsupported until an adapter exists; the program must not silently claim parity with them.

## Recommended first implementation sequence

1. Correct NTP privilege handling and provider detection; fix false Action Center/firewall/update states; withhold cosmetic writes and unsupported Ubuntu package actions. These are concrete correctness fixes before new settings are added.
2. Introduce the action catalog, persistent jobs and settings lifecycle; migrate the existing color-scheme page as a small real integration and add search/icon views/accessibility. Preserve working behavior while replacing text-based routing.
3. Prove Wayland display/revert, PowerDevil, nested Aero settings and APT-provider feasibility; expand the module snapshot into reviewed per-setting acceptance tasks. Use those results to estimate and schedule the remaining milestones.

The effort is a collection of settings-domain implementations and compatibility work. A calendar estimate should follow the feasibility checks and the per-control inventory, rather than being inferred from the current number of pages or installed KDE modules.


### Batch 17: guarded display layout and capability-driven power policies

The persistent display editor stages connected-output mode, enablement, logical
arrangement, scale and orientation as one guarded candidate. Its independent
watchdog verifies Keep and eligible partial rollback, preserves unrelated display
properties, and rejects stale topology/provider changes. Native Wayland discovery
fixes the validation-only inherited offscreen platform failure.

Power Options adds native idle action choices, dimming/brightness/backlight,
locked-display policy, power-button/lid actions, suspend modes and global battery
levels/actions, conditional on exact provider capabilities and entry policy.
Owner/capability preflight and acknowledged reload/readback retain uncertain
outcomes until explicit recovery. The Aero controls share the existing sessions
and navigation guards. Mirroring/preferred-mode editing, HDR/color/VRR, resets,
activities, further provider controls and real effects/persistence acceptance
remain open. See [Batch 17 scope](kde-batch17-display-power.md).


### Batch 18: explicit power values, native defaults and custom entry

Power drafts retain per-key Set/Restore Default intents and current/default
knowledge separately. Supported explicit values remain available when the
PowerDevil daemon's effective fallback cannot be proved. Independent controls
preserve their untouched companions. Scoped defaults reviews pin both saved and
staged state and follow native inherited/user-global/no-op semantics; saved-value
readback and reload acknowledgement remain required. Custom timeout entry uses
native minima and a one-week maximum, with distinct sentinel/default choices.

Display custom scales preview native rounding, preserve untouched raw values and
retain exact rollback requirements. Signed position entry extends through
±1,000,000 logical pixels before native normalization/bounds checks. KScreen has
no factory-default operation; Revert reloads/restores verified current settings.
Mirroring/preferred modes, HDR/color/VRR, further power providers and complete
installed-module reconciliation and real-session acceptance remain open. See
[Batch 18 scope](kde-batch18-defaults-custom-values.md).


### Batch 19: Night Light filter and runtime observations

A native Aero page stages the installed four-key Night Light configuration,
independent day/night temperatures and explicit native defaults. Source/policy
provenance, provider identity/epoch, untouched raw values, failed-Revert drafts
and shared navigation/background jobs remain guarded. Native Notify saving and
fresh readback are distinct from the available public enabled/mode/target
observations; no full loaded-temperature acknowledgement exists. Explicit
15-second previews have bounded, best-effort owner/epoch-pinned cleanup.

The linked Day and Night module has separate schedule, location/privacy and
KNightTime daemon behavior. Its native forms remain incomplete; preview and
provider construction are never used as an inert read probe. Real filter effects,
notification adoption, physical output and sign-in persistence remain open.
See [Batch 19 scope](kde-batch19-night-light.md).


### Batch 20: Day and Night schedule and location

The linked native Aero page now stages all seven schedule/location preferences,
with an offline map, native forecasts and explicit app-owned one-shot location.
Separate FullConfig editor and NoGlobals daemon inputs preserve native defaults,
policy and raw values; coupled edits are atomic and the complete active daemon
branch is validated before Notify. A persistent Apply session survives navigation.
Inert discovery never activates/subscribes to KNightTime or constructs a location
provider. Real acquisition/authorization, native legacy migration, reload adoption,
physical consumer behavior and sign-in persistence remain unverified; this does
not declare whole-module or full installed-KDE completion.
See [Batch20 scope](kde-batch20-day-night.md).


Batch 21 adds the bounded native [Screen Locking policy workflow](kde-batch21-screen-locking.md),
with five owned scalar preferences, native defaults, timer safety, existing
shortcut-editor routing and passive approved Aero appearance evidence. Shared
privileged SDDM/Aero background editing, real locking/authentication/resume and
sign-in acceptance remain open. No whole-module completion is inferred.

Batch 22 adds the bounded [Aero login and shared lock appearance workflow](kde-batch22-aero-login.md),
including the four custom login booleans, source-correct SDDM inputs, explicit
image intents, unprivileged image preparation and validation, a new fixed
authorized transactional helper and reviewed recovery. Host assets are verified
root-owned; the editor remains read-only pending trusted system helper installation. Real authorized writes,
rendered greeter/lock adoption, authentication and persistence remain open.
Batch23 addresses Screen Locking's shared KConfig UI-thread contention issue
with value-only workers, persistent jobs/service ownership, retained file
reservations and soft progress deadlines. It also prevents implicit Font and
Keyboard retries after failed synchronization. Other synchronous writers and a
common cross-provider file reservation still need migration. Native post-lock
merge behavior and live write/adoption acceptance remain distinct limits. See
[Batch23 lifecycle scope](kde-batch23-save-lifecycle.md).

Batch 24 migrates Night Light and Day and Night file work to application-owned
workers and shares canonical/inode reservations with Screen Locking, including
their user kdeglobals overlap. Slow saves remain busy through actual completion;
native Notify broadcasts and post-lock merges retain their uncertainty limits.
These lifecycle changes preserve existing controls rather than add module
completion. See [Batch24 scope](kde-batch24-shared-file-jobs.md).

The user requested a pause after Batch 24 and repository persistence. Resume from
the current audited backlog: remaining synchronous providers and reservation
adoption, plus unresolved native control/provider coverage and real-session
acceptance. The approved Aero-only product boundary remains in force. The Aero
login assets are already root-owned; its new administrative helper still needs
a trusted system installation before live editing. Full parity remains open.
