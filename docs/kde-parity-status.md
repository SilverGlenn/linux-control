# KDE capability parity status — 17 October 2026

Full native Aero Control Panel parity is **not complete**. The current implementation contains useful, bounded native editors; most installed KDE settings still need native controls, provider integration, defaults/reset behavior and real-session acceptance. An external KCM or setup wizard provides access only and earns no native-parity credit.

The release product supports the approved custom Aero theme. Alternative theme/component selection is explicitly excluded. Tuning approved Aero components, fonts, wallpaper, login/lock policy and unrelated KDE preferences remains in scope. Missing implementation is never evidence that hardware is inapplicable.

## Frozen inventory

The machine-readable [capability backlog](audits/kde-capability-backlog.json) expands the unchanged [baseline](audits/kde-settings-baseline.json): **106 modules and 26 nested configuration plugins** are all represented. It records **666 capability and unresolved-obligation rows**:

| Record type | Count | Meaning |
| --- | ---: | --- |
| Installed schema entries | 236 | Actual keys, types, groups, defaults/expressions and choices read from installed schemas |
| Installed writable device properties | 30 | Exact KWin InputDevice fields; 20 physical pointer fields now have bounded Batch 10 source/fixture coverage |
| Installed provider operations | 21 | Selected real AccountsService, KWin and bolt operations; safe UI mapping remains work |
| Bounded implemented controls | 151 | Named source/fixture evidence from Batches 4–24; real writes remain unverified |
| Product exclusions | 13 | Explicit alternative-theme/decoration boundaries |
| Discovery groups | 72 | Tasks whose exact visible controls still need source/UI reconciliation |
| Remaining coverage obligations | 143 | Every module/plugin retains an unresolved coverage row; nine Batch 8 and two audio gaps are explicit |

These counts are **not a completion percentage or a unique-setting denominator**. Schema entries can overlap implemented controls, provider properties can serve multiple KCMs, and internal schema keys need visible-control mapping. Custom C++/QML forms, dynamically loaded providers and script settings may add controls. Each row has a stable ID, module/provider target/version, evidence pointers, applicability reason, and separate native UI, read, write, defaults/reset, external refresh, error and runtime statuses. Installed schema/project evidence is fingerprinted. Package-managed plugin versions come from the local dpkg database; unpackaged Aero plugin hashes are recorded with extension versions unresolved rather than inferred from Plasma's version.

There are **zero whole modules declared complete, zero live write verifications and zero hardware exclusions** in this audit. The three nonapproved nested decoration providers are product-excluded. Other modules retain unresolved obligations even when some of their controls are implemented.

## Current bounded evidence

| Area | Native/source evidence | Still blocks full parity |
| --- | --- | --- |
| Aero appearance | Versioned approved identities/palette restoration; glass color/HSV/intensity/transparency/reflection/glow/blur, cursor sizes, two SMOD options | Remaining SMOD/Aero custom forms, AeroShell SDDM and other approved extension controls; real adoption/persistence acceptance |
| Wallpaper/fonts | Local image and fill modes on existing desktops; six font roles and supported fontconfig overrides; user-font inventory/install/removal within the app-managed directory | Slideshows/dynamic wallpaper, DPI/X11 export, advanced font rules, system/unmanaged font management and rendering acceptance |
| Display Batches 17–18 | Guarded connected-output fixed modes, enablement, signed arrangement, fractional/custom scaling, raw-value preservation, four orientations and whole-layout partial-origin rollback | Mirroring/preferred-mode editing, primary ordering, color/HDR/VRR and other display controls; no native factory-reset operation exists; real multi-monitor/recovery acceptance |
| Night Light Batches 19 and 24 | Four-key filter/default workflow, independent temperatures, explicit preview/Stop lifecycle and reported runtime/timeline; persistent worker file jobs, common reservations and bounded unsent-preview retirement | Exclusive preview ownership (native API has no cookie), causal reload adoption, physical appearance and persistence acceptance |
| Day and Night Batches 20 and 24 | Seven-key schedule/default workflow, dual editor/daemon views, offline map, native forecast and bounded explicit acquisition; persistent worker file jobs and common reservations | Actual location authorization, legacy migration, Notify adoption, running consumer effects and persistence acceptance |
| Screen Locking Batches 21, 23 and 24 | Five-key policy/default workflow, timer safety, worker file jobs, retained service/activity, common file reservations, pinned configure acknowledgement and existing shortcut routing | Remaining providers' reservation adoption, real locking/authentication/resume and sign-in acceptance; shared appearance is separately bounded in Batch 22 |
| Aero login/lock appearance Batch 22 | Four native login options, bounded local PNG/JPEG preparation, explicit shared-background remove/restore, strict newly authorized file transaction and recovery, persistent drafts and review | Trusted system helper installation; authorized live writes/recovery, rendered consumer adoption and sign-in persistence; host assets are already root-owned |
| Power/window behavior Batches 17–18 and 25 | Conditional idle/display/dim/brightness/backlight/locked-display/button/lid/sleep-mode/battery policies, per-key defaults and provenance, scoped defaults review and custom timeout domain; persistent worker saves, common file reservations, guarded reload and honest cancellation; title-bar double-click action | Activities/inhibitors/scripts/performance/charge/peripherals and additional power controls; most workspace behavior and real effects/persistence |
| Accounts/date/time | Current local-user display name; date/time/timezone and Chrony/timesyncd handling | Full account administration and fingerprint mapping; real NTP/helper authorization and persistence acceptance |
| Network Batch 8 | Managed Ethernet/Wi-Fi overview, scan, supported discovered Wi-Fi connection, compatible saved activation/disconnect, selected IPv4/IPv6/profile properties, checkpoint Keep/Revert | VPN/proxy/hotspot/cellular, hidden/enterprise/WEP/OWE Wi-Fi, routes, radios, profile creation/deletion, advanced properties; real credentials/auth/recovery acceptance |
| Audio Batch 9 | Native endpoint and application defaults/volume/mute/routing, ports, card profiles and channel levels; asynchronous preflight/readback and lifetime identity | Global over-amplification preference, speaker tests/meters, event sounds/call ducking, advanced formats/policy and real writes/persistence; pactl connection-generation race |
| Mouse/Touchpad Batch 10 | Twenty typed KWin physical-pointer preferences, mode coupling, per-device reported defaults, staged Apply/Revert, external conflicts and bounded uncertainty handling | Shortcuts, tablet/touchscreen, mouse binding/workspace preferences, config-driven scroll default and real physical/sign-in acceptance |
| Keyboard Batch 11 | Native repeat/delay/rate, startup Num Lock, model, ordered layouts/variants/labels, policy, XKB options and layout OSD; native KConfig notifications and saved-file readback | Additional preview geometry, runtime effects and sign-in acceptance; Wayland loop subsets preserved read-only; Batches 13–14 extend previews and configured per-layout shortcuts |
| Default Programs Batch 12 | Twelve KDE roles, compatible MIME/protocol handler selection, scoped recommendations, multi-file conflict/readback and policy handling | Complete association ordering/editor/embedding controls, running-app/cache adoption and real persistence acceptance |
| Shortcuts/Preview Batch 13 | Registered global component/context/action bindings, full alternatives/chords, staged reassignment/defaults, native persistence and disk conflict checks; compiled symbols/all levels and source-grounded PC geometry | Scheme import/export, custom launch/command creation/removal, arbitrary-model geometry and live keyboard/inhibition/persistence acceptance; Batch 14 adds common application bindings and layout activation |
| Application/Layout Shortcuts Batch 14 | Installed standard-action catalog, fresh layered policy reads, scoped alternatives/defaults/conflicts, native file transaction/notification; translated runtime-layout mapping, configuration-only registration and pinned KWin reload acknowledgement | Application-specific overrides/adoption, physical invocation and sign-in persistence remain unverified; schemes, command/application creation/removal and complete module reconciliation remain open |
| Shortcut Schemes/Custom Commands Batches 15–16 | Native schemes, atomic binding/definition drafts, retained dependency review/resume, installed-application registration, managed command editing, guarded whole-component removal across contexts and unfinished-import close protection | Edited-command daemon adoption, arbitrary action deletion/context activation, full provider reconciliation and live invocation/persistence acceptance |
| Other installed domains | Installed accessibility/workspace/KWin/KRunner/audio-CD schemas and selected provider APIs are expanded into actual fields | Native editors for input, regional/accessibility, devices, applications, notifications/search, login/locking, privacy/security/backup and remaining extension forms |

The detailed backlog distinguishes precise installed schema/API facts from inferred discovery groups. In particular, its input device fields do not assert that every property is offered by every input device or visible in every KCM.

## Evidence limits

The recorded Batch 7 probe reports a display-discovery timeout with zero returned outputs, and Font Apply is disabled in that captured environment. Batch 17 reproduces the inherited offscreen-platform cause and verifies fresh read-only Wayland display discovery. The historical observation is preserved; no guarded display write/recovery acceptance or new Font Apply acceptance is inferred. Other read-only observations include the approved Aero contract, account, wallpaper and PowerDevil profile data. The older observations remain historical; fresh read-only keyboard/default-program probes are recorded below.

The Batch 8 read-only NetworkManager probe reports version **1.54.3**, three devices and three saved profiles. Connectivity checks were unavailable and disabled. Recorded fixtures and documentation cover supported checkpoint/property paths, while real connection changes, secret-agent interaction, authorization and recovery remain unverified.

Batches 11 and 12 add fresh read-only discovery: KWin 6.6.6 returns the existing
English (Colemak) layout; the installed registry has 184 models, 104 layouts and
23 option groups. Default Programs finds 205 desktop entries, 1,018 MIME names
and 69 valid saved preferences. Both providers return stable repeated reads.
All 61 fixture suites passed (28.46 seconds), the Aero captures were inspected
and the staged install passed. Hashes of 31 relevant live configuration paths
were unchanged. No live setters, reloads, launches or cache rebuilds were used;
real preference changes and adoption remain unverified.

Batch 13 adds stable repeated read-only discovery of 277 global shortcut
actions across 22 components. All 65 fixture suites pass (28.96 seconds), the
native Aero shortcut and compiled-layout preview captures were inspected,
and a staged install passed. Five monitored keyboard/shortcut configuration
files remained unchanged. Shortcut invocation, recording inhibition, live
writes and persistence across sign-in remain unverified.

Batch 14 adds 84 standard application actions across six categories. Every
binding matches the installed SDK in a fresh process, with zero mismatches or
probe-specific application overrides. Repeated global/standard snapshots are
stable; the verified KWin 6.6.6 adapter recognizes the configured English
(Colemak) per-layout action. All 66 fixture suites pass (32.00 seconds), native
Aero global/standard captures were inspected, and install staging passed. Five
monitored live configuration files remained unchanged. No live registration,
reload, save, invocation or recording was performed; application adoption and
physical/sign-in acceptance remain open.

Batch 15 adds scheme review/export and staged custom shortcut management. All
71 fixture suites pass (28.98 seconds), five native Aero captures were inspected,
and install staging passed. Read-only production discovery returns 205
applications, 12 registered application components and 277 shortcut actions;
the audited KWin-embedded provider is writable. Final repeated custom/global/
standard snapshots match, and all 84 standard actions match the installed SDK.
Of 26 monitored live paths, 25 remained unchanged and none were added.
`kglobalshortcutsrc` changed during the read checks; three Spectacle action labels
differed between snapshots, while all observed bindings/defaults stayed equal.
Complete file-level attribution is unverified because pre-change bytes were
not retained. Validation performed no live save, registration, cleanup, reload,
invocation, recording, launch or cache rebuild. Automatic imported-command
coordination, multi-context removal and edited-command adoption remain open.

Batch 16 extends that baseline with retained import dependency coordination,
guarded whole-component removal across contexts and atomic existing-command
editing/registration. All 72 suites pass (28.38 seconds), the retained review
captures were inspected and install staging passed. Read-only discovery finds
205 applications, 12 registered application components and 22 complete context
inventories; global actions remain 277 and standard actions remain 84 with zero
SDK mismatches. Repeated snapshots match, and all 26 monitored fingerprints are
unchanged with no new entries/cache files. No live mutation or invocation was
performed. The source audit confirms that edited Exec is cached by an existing
service component, so cache refresh alone does not prove execution adoption.
That adoption, real Apply effects and sign-in acceptance remain open.

Batch 5 identifies an Aero source checkout at `repos/aeroshell-kwin-components`, commit `97cc934b54aa9b237a8b814cb86f93f7d33b5ebf`. That path is absent at the currently inspected AeroThemePlasma checkout. The versioned project contract, plugin fingerprints and fixtures remain evidence for their documented subset; unseen extension controls are explicitly unresolved.

The capability audit performs no live writes. Implementation evidence is bounded by
[Batch 8 networking](kde-batch8-networking.md), [Batch 9 audio](kde-batch9-audio.md),
[Batch 10 input](kde-batch10-input.md), [Batch 11 keyboard](kde-batch11-keyboard.md),
[Batch 12 default programs](kde-batch12-default-apps.md), [Batch 13 shortcuts/preview](kde-batch13-shortcuts.md),
[Batch 14 standard shortcuts](kde-batch14-standard-shortcuts.md) and
[Batch 14 layout activation](kde-batch14-layout-shortcuts.md),
[Batch 15 schemes](kde-batch15-shortcut-schemes.md) and
[Batch 15 custom shortcuts](kde-batch15-custom-shortcuts.md) and
[Batch 16 retained imports/removal](kde-batch16-shortcut-imports.md),
[Batch 17 display/power](kde-batch17-display-power.md) and
[Batch 18 defaults/custom values](kde-batch18-defaults-custom-values.md) and
[Batch 19 Night Light](kde-batch19-night-light.md),
[Batch 20 Day and Night](kde-batch20-day-night.md) and
[Batch 21 Screen Locking](kde-batch21-screen-locking.md); none establishes full-module parity.

## Next safe implementation groups

1. Complete edited-command adoption, remaining keyboard model geometry and exhaustive shortcut/provider reconciliation, including applicable arbitrary action deletion/context activation. Retained import dependencies, guarded whole-component removal, schemes, managed definitions, registered globals, configured per-layout creation/reload, standard bindings and compiled previews have bounded implementation; live acceptance remains open.
2. Complete remaining audio/input capabilities and real-session acceptance; preserve the documented provider and defaults limitations.
3. Complete display mirroring/preferred modes, primary ordering, color/HDR/VRR and remaining controls; add power activities/inhibitors/scripts/performance/charge/peripheral policies and reconcile mobile frontends. Guarded layouts and per-key explicit/default/custom operations have bounded implementation. Device absence requires actual capability evidence. Batch19 adds bounded Night Light filter/default/preview coverage; Batch20 adds native Day and Night forms/map/forecasts and explicit app-owned acquisition, while actual location authorization, migration/adoption and physical/persistence acceptance remain open. Inert probes do not activate or subscribe to the daemon.
4. Complete AccountsService tasks, locale/accessibility, locking, chrony, Bluetooth and printer/device adapters with truthful authorization, cancellation and service-loss outcomes.
5. Implement workspace, startup/login, applications, notifications/search, wallet/privacy/firewall/backup and each remaining approved extension custom form. Reconcile duplicate mobile frontends with shared capabilities.

For each applicable capability, acceptance must independently establish native read, stage/cancel, apply, observed live effect and persistence, reread/KDE agreement, defaults/reset, external-edit refresh/conflicts, errors/authorization cancellation, service/device loss and release mismatch. A Revert button does not prove defaults/reset. Fixtures and successful reload notifications do not establish rendered or real-device effects. All unresolved applicable rows continue to block a full-parity claim.


Batch 17 adds seventeen bounded display/power scopes while preserving all 644
prior row IDs, evidence and unresolved obligations. Display recovery proves a
single shared native origin shift across a partially applied multi-output layout;
foreign coordinates remain ineligible for automatic restoration. Power writes
pin the native owner and capability revision, preserve unknown raw values and
require reload acknowledgement plus fresh readback. Failed Revert reads retain
the draft and stop pending navigation.

Repeated production reads discover one eDP-1 panel with eleven modes, verified
KWin 6.6.6/libkscreen 6.6.4 identity and available guarded layout/scale/rotation
support. PowerDevil reads and both battery thresholds are stable. Keyboard
backlight is not advertised. The pinned daemon environment is unreadable in this
session, so absent mobile-dependent defaults disable the affected controls,
including the short-press power button and some battery timeout fields. This
remains an explicit default-presence/override adapter gap, rather than a hardware
exclusion. Real button, lid, brightness, suspend and display recovery effects
remain unverified. See the Batch 17 report for final execution evidence.


Final Batch 17 validation passes 73 CTest suites in 30.50 seconds. All five new
fake-provider Aero captures were inspected, final build/install staging passes,
and all 31 monitored live fingerprints are unchanged. Production discovery is
read-only; zero live write verifications, whole-module completions and hardware
exclusions remain in force.


Batch 18 preserves all661 preceding row IDs and adds one bounded cross-key
workflow (662 total/147 implemented scopes). It separates explicit values from
native-default operations and current/default knowledge, implements reviewed
scope resets and native custom timeout/scale/position entry, and preserves raw
values and untouched companions. Reset controls reflect actual operation
availability; staged values and unknown checkboxes remain visually distinct.

The final build and all73 CTest suites pass (95.17 seconds), eight Aero captures
were inspected, and DESTDIR staging passes. Repeated read-only discovery returns
one panel/eleven modes and54 power-key records; all editor defaults are known,
while eleven effective values remain unknown and supported explicit replacements
remain available. Keyboard backlight is not advertised. No live setter/reload or
power action was invoked by the audited workflow.

Of31 monitored live paths,29 remain unchanged and none were added. Two KWin
changes remain unattributed: stored brightness0.75→0.70 in kwinoutputconfig.json,
and VirtualKeyboardEnabled=true added to kwinrc. Indirect runtime effects are
not excluded; both files were preserved with private before/after evidence.
This drift is not accepted live-write verification. Zero whole-module completion,
verified live-write or hardware-exclusion claims remain in force. See
[Batch18 evidence and limits](kde-batch18-defaults-custom-values.md).


Batch 19 retains all 662 preceding IDs, evidence and module statuses and adds one
bounded native Night Light workflow (663 total/148 implemented scopes). The
four-key filter, native defaults, explicit preview/Stop, runtime observations
and keyboard/input/navigation lifetimes have source/private-fixture evidence.
The linked Day and Night schedule/location module remains incomplete.

All 77 final CTest suites pass in 110.55 seconds; five Aero captures, final
no-work build and five-file DESTDIR staging pass. Repeat getter-only reports
match: KWin 6.6.6 owner :1.15, four known/default-known writable keys, enabled
Dark/Light mode, day6500/night4000 K and current/target6500 K during daytime.
Saved discovery and current public observations do not prove both temperatures
were loaded, a causal reload, physical color or sign-in persistence.

Final byte fingerprints show 29/31 unchanged and no new application/cache files.
Stored brightness changed0.70→1.0 before automated build/tests/probes; a later
kdeglobals BrowserApplication line moved within General without changing its
value. Both changes remain unattributed and were preserved with private
evidence; indirect runtime effects are not excluded. No live preference setter,
reload, preview/Stop, inhibition, Nighttime Subscribe/location, registration,
cache rebuild or live install was invoked by the audited workflow. Full parity
and all real acceptance remain open. See [Batch19 scope and evidence](kde-batch19-night-light.md).


Batch 20 retains all 663 preceding IDs, evidence pointers and module statuses and
adds one bounded native Day and Night workflow (664 total/149 implemented scopes).
Native configuration, default/provenance and forecast behavior have source and
private-fixture evidence. Explicit acquisition has fake-provider evidence only;
real location, migration, adoption and persistence acceptance remain open.
See [Batch20 evidence and limits](kde-batch20-day-night.md).


Final Batch20 evidence: 83 suites pass in 116.91s, 26 focused page results pass,
six final Aero captures were inspected, final Ninja build has no work, and
five-file DESTDIR staging succeeded. Two inert reports match KNightTime6.6.5
owner:1.17/API1, with seven known/default-known writable keys in both views and
an age-unknown cache (coordinates redacted). Fingerprints retain32/34 bytewise;
one service cache and the location state's Latitude already changed before
build/tests/probes. These unattributed changes were preserved, with no new
application/service-cache files. No live-write, acquisition, adoption, migration,
physical behavior or persistence acceptance is claimed.


Batch 21 adds native Screen Locking policy/defaults, existing shortcut routing and
passive approved Aero appearance evidence. All 88 suites pass (166.42 seconds),
five Aero captures were inspected and five-file install staging passed. Four
read-only native snapshots match: nine known fields, five editable preferences,
and matching Aero shell/provider/background bytes. Both later probe intervals
preserved all 51 monitored fingerprints. A targeted fourth-probe file trace found
no writable opens or file mutation calls; its expanded 54-path inventory stayed
unchanged. This does not exclude indirect effects. Across the full interval, 47 final
fingerprints match baseline; two service caches, a display configuration and a
cached location state differ, with an additional transient shortcut-label change
that returned to baseline. Final inventory also found two previously unmonitored service caches and a
System Monitor desktop entry. Private bytes preserve the evidence; creation times,
attribution and indirect effects remain unresolved. No live setters, configure, Lock, inhibition,
registration/invocation, cache rebuild, privileged helper or live install ran.
Shared privileged Aero appearance editing and real lock/auth/resume/sign-in
acceptance remain open. See [Batch 21](kde-batch21-screen-locking.md).


Batch 22 adds the fixed Aero login and shared lock-picture workflow. All 95
suites pass (166.78 seconds); six fake-provider Aero captures and normal/stripped
eight-file staged installs were verified. Both approval manifests match the final
installed helper bytes. Two native passive snapshots match: four known booleans,
trusted root-owned host assets and no new system helper. Paired metadata confirms
that the sandbox maps root IDs to nobody; there is no host ownership defect.
The editor remains read-only pending trusted system helper installation.

The probe interval preserves all56 monitored fingerprints and inventory paths;
its file trace shows only private INI scratch operations. Across the full work
interval,47 final fingerprints match and9 differ, seven already before the first
build. Private bytes preserve the four cache, shortcut/display, two Explorer
launcher and cached-location differences; attribution remains unresolved. No
live setter, helper authorization, greeter, locking or installation ran. Real
write/recovery/rendering/persistence acceptance and full installed-KDE parity
remain open. See [Batch22](kde-batch22-aero-login.md).


Batch 23 moves Screen Locking file reads/saves/guards to value-only workers and
keeps app-owned activity, service and provider-local canonical/inode file
reservations through terminal completion, including after backend destruction.
Soft progress preserves busy drafts; replacement-inode aliases and duplicate/late
configure guards have isolated coverage. Font/Keyboard failed synchronization now
cleans unsaved dirtiness before callbacks/destruction; Font XML partial saves
remain explicit. Other synchronous providers and common cross-provider file
ownership remain migration work. Native post-lock merging and sent-call late
execution remain limits.

All 96 final suites pass (169.12 seconds) against 403 frozen source/test/build
inputs; six final fake-provider Aero captures and both eight-file staged installs
were verified. Two matching passive native snapshots report nine known fields
and five writable capabilities; both file traces show zero writable opens or
path mutations. Both probe intervals preserve all 56 monitored fingerprints and
expanded inventory paths. Across the work interval, 55 final fingerprints match;
cached KNightTime state already differed before the first build and is the only
observed drift. Private evidence retains that difference without attribution.
The backlog retains all 666 prior IDs, statuses and evidence, 151 implemented
scopes and the complete module inventory. There are no added whole-module, live
write or hardware-exclusion claims. See [Batch 23](kde-batch23-save-lifecycle.md).


Batch 24 moves Night Light and Day and Night file work to value-only workers and
shares target/global canonical/inode reservations with Screen Locking. App-owned
read/write activity and service ownership survive session destruction; soft
progress retains busy drafts. Unsent preview preparation retires on Stop, expiry,
provider replacement, accepted Apply and destruction; awaiting observers finish
without a remote-cleanup claim and newer reentrant requests retain ownership.
Native Notify broadcast, post-lock merge and already-sent execution remain limits.

The final Debug build and all **100 CTest suites pass (181.90
seconds)** against 409 frozen source/test/build inputs. The final 15-suite focused
run passes in 37.26 seconds, including shared admission/cross-provider jobs,
preview retirement/reentrancy, both controllers, asynchronous providers,
navigation and Close. The private native Screen Locking lock-wait regression
remains in the full suite. Final Ninja validation reports no work.

Two fake-provider Aero pending-save captures were inspected. They assert actual
staged differences, busy drafts, progress and disabled Apply/Revert. Normal and
stripped DESTDIR installs each contain eight files; helper/policy approval
manifests match the final staged bytes. No live installation ran.

Two passive native snapshots per provider match: Night Light reports four known
writable keys; Day and Night reports seven known writable editor keys and seven
known daemon inputs; Screen Locking reports nine known fields and five writable
preferences. All six process/child file traces contain zero writable opens or
path mutations, and all six probe intervals preserve every monitored fingerprint
and expanded inventory entry. These are getter observations, not runtime
adoption or write acceptance, and the traces do not exclude indirect effects.

Across the complete work interval, **54/56** final byte fingerprints match.
Cached KNightTime state already differed before the first build; stored display
brightness differed before the final build. Both changes remain unattributed,
with private before/after bytes preserved. No live setter, Notify/configure,
preview/Stop, location activation/acquisition, locking/inhibition, registration,
launch, helper authorization or live installation was used by validation.

The backlog preserves all 666 IDs, statuses and earlier evidence, 151 implemented
scopes and all 132 module/plugin inventory entries. There are zero whole-module,
live-write or hardware-exclusion claims. Private execution artifacts are retained
under `build/batch24-validation` and excluded from Git.

The user requested a pause and repository checkpoint after this batch. Remaining
worker/reservation migrations are listed in the lifecycle audit; full native
capability coverage and real-session acceptance remain open. See [Batch24](kde-batch24-shared-file-jobs.md).

Batch 25 moves Power Policy file work to persistent workers and adopts the shared
target/global reservations. Daemon/activity ownership survives backend deletion;
slow saves retain busy drafts. Cancellation waits for actual native file work,
and fresh source/provider guards check unsent reload dispatch. Native post-lock
merging and sent-call late execution remain limits. Existing 17 profile and
three low/critical battery warning/action preferences remain separate from
charging-limit controls.

All **101 suites pass in 191.94 seconds**, including the ten focused suites
(22.84 seconds) and 18 cross-provider ownership cases, against 410 frozen inputs.
The final Ninja invocation reports no work. One Aero pending-save capture and
both eight-file normal/stripped staged installs were verified; no live install
or power/charging setter, reload or physical power action ran.

The repaired charging policy remains enabled after the October 25 reboot, with
recent history capped at 80% and a passive observation of 79%, Not charging on AC.
All three charging-policy fingerprints remain unchanged through validation.
Across all 59 monitored paths, 57 final fingerprints match; KNightTime cache
drift preceded the first build and KSycoca cache drift first appeared during
compilation. Both remain unattributed. The original charging-limit disabling
actor remains unproved.

All 666 backlog IDs, statuses and previous evidence, 151 implemented scopes and
132 module/plugin inventory entries are preserved. No whole-module, live-write
or hardware-exclusion credit is added. Remaining worker migrations and native
acceptance are tracked in [Batch 25](kde-batch25-power-file-jobs.md) and the
[lifecycle audit](audits/kconfig-sync-lifecycle.md).
