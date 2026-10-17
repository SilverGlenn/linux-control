# Batch22 Aero login/lock appearance audit — discovery only

No repo edits, build/test, helper/auth/greeter execution, image preview, QML/plugin
construction, provider activation, preference write, configure, lock, or inhibition.
Only source/package/file metadata and bytes were inspected. Source downloads are
under this directory. Installed evidence hashes/UID/mode/mtime are in
`installed-evidence.json`; this records evidence, not executable/source equivalence.

## Exact sources and proof boundaries

* Local clean aeroshell-sddm-kcm HEAD `8c94283c2689b3f0837efda0be8107e42a265e0f`,
  origin https://gitgud.io/aeroshell/aeroshell-sddm-kcm.git; copied source snapshot
  in `aeroshell-sddm-kcm/`. Native KCM carries no release/version metadata.
* Installed KCM SHA256 `83e1cc08d45560df34c4e073e51d2e471a349956fd5b994d14de39ccd552b179`,
  ELF build ID `f1cea0481c1aa9efd0079cdd3c4383f95fc7a267`.
* Installed helper SHA256 `760aabf35c8faf6b8cf3b1448681f18ded04d9bbea837bce1c7b83bb3c931078`,
  ELF build ID `1983b0656fc26af1feed3df19b887121e1e6d935`.
* Neither KCM/helper nor theme Main.qml is owned by a dpkg package. Binary hash
  pins observed bytes, not a reproducible source build or trustworthy installation.
* Reader source in `aeroshell-reader/` comes from local aeroshell-workspace HEAD
  `ea5dfad53b09031b6804124fa3e938ba373bca45`, origin
  https://github.com/aeroshell-desktop/aeroshell-workspace.git. Its working tree
  has a CMakeLists modification; reader-source/binary equality remains unproved.
  Installed reader SHA256 `0c6cb27868f7aea8b62a5ebe0920343b506525a0c3c92e4047636424cdcb16f4`.
* Installed SDDM package `0.21.0+git20250502.4fe234b-2ubuntu3`. Relevant official
  upstream baseline commit https://github.com/sddm/sddm/tree/4fe234b is cached
  in `sddm/`; distribution patch/build equivalence is not established by this audit.
* Qt bool conversion confirmed against official Qt6.10.2 qmetatype.cpp,
  `convertToBool`, cached here. KF KConfig6.24 copyFrom source remains cached in
  `/tmp/batch18-power-audit/kconfig.cpp` (marks all copied entries/config dirty).

## Whole visible native custom form

`src/ui/main.qml` has only Apply; it does not expose native Defaults/Revert.
The KCM header overrides only load/save. No native restore-default implementation
exists. `checkRepresentsDefaults` is connected by string but not implemented here.

1. Theme to configure picker: all readable system `/usr/share/sddm/themes/*`
   directories having nonempty General/aeroShellDisplay in theme.conf. This picks
   a configuration target, not the active SDDM theme. Product must pin only
   `/usr/share/sddm/themes/sddm-theme-mod` and preserve active theme selection.
2. Enable Vista Startup Pearl Animation — `General/enableStartup`, Bool.
3. Play Startup sound — `General/playSound`, Bool.
4. Force user selection — `General/forceUserSelect`, Bool.
5. Use RDP background — `General/rdpBackground`, Bool; disables background UI.
6. Background preview and Change... local file chooser, PNG (*.png), JPEG
   (*.jpg,*.jpeg). The existing helper does not validate image data or bounds.
7. Apply submits all four booleans, caller-provided theme path, and selected URL.

Installed base theme.conf has false/true/false/false for the four booleans,
plus `aeroShellDisplay=AeroThemePlasma` and `startup=false` (unowned/unused by
this form). There is currently no theme.conf.user. Base and override presence,
raw spellings, unknown keys, and other themes must remain distinguishable.

## Actual parser/default semantics: do not conflate views

Native KCM opens theme.conf with KSharedConfig default FullConfig. If user file
exists it calls copyFrom(KConfig(theme.conf.user)): this REPLACES entries with the
user-config entry map, not the SDDM overlay, marks entries dirty, and clears file
immutability. Merely opening the KCM can attempt to write the base file later;
never construct it as a passive reader.

KCM load fallbacks are false,true,false,false. Its dirty-check enableStartup
fallback is incorrectly true. Helper compares only user file values using
false,true,false,false, then writes changed booleans to theme.conf.user using
KConfig. Do not replicate these bugs as effective SDDM configuration semantics.

SDDM ThemeConfig.cpp uses QSettings INI, base file then `.user`; it overlays ONLY
user values whose `toString()` is nonempty. `boolValue` uses QVariant::toBool.
No KConfig global/cascade/Kiosk policy interpretation applies in this consumer.
For a string, Qt conversion is false only for empty/"0"/case-insensitive"false";
"no" and "off" are TRUE here but FALSE to KConfig. Unknown/unusual raw values
must remain visible, with editor/runtime disagreement explicit. Canonical new
writes should use true/false. Empty user values do not hide base values.
`[General]` is QSettings' root/general section, not an arbitrary nested prefix.

A product Restore Approved Defaults can reset owned overrides to pinned base
values, but must be labelled as product functionality, not a native KCM button.
Resolve unknown/default raw values explicitly; preserve unowned bytes/keys.

## Consumer effects and lifetime

SDDM installed metadata: Theme-Id=sddm-theme-mod, Theme-API=2.0, QtVersion=6,
ConfigFile=theme.conf, MainScript=Main.qml. The SDDM Main.qml consumes:

* enableStartup: pearl animation and sound start delay. Animation/sound also use
  `/tmp/sddm.startup` (ls/touch via executable datasource), so true does not imply
  it will play on every login/greeter activation.
* playSound: startup sound `Assets/session-start.wav`, gated by that marker.
* forceUserSelect: bypasses automatic single-user login-prompt selection.
* rdpBackground is NOT read by installed Main.qml. Its visible effect comes from
  helper deleting `background`.
* Login background is hardcoded `Qt.resolvedUrl("background")`; the component
  does not fall back to `default-background`. Under it is #1D5F7A rectangle.

Installed Aero lock AuthUI consumes none of the four theme booleans. Its unlock
sound reads the user's KDE global sound theme. It consumes only
`aeroshell.utils.SDDM.currentBackground`: custom reader opens SimpleConfig
`/etc/sddm.conf.d/kde_settings.conf`, Theme/Current default sddm-theme-mod;
uses last located system sddm data directory; selects background, else
`default-background`, else theme.conf.user + theme.conf General/background.
It watches only kde_settings.conf, not image/theme config files. Thus removing
background can yield BLUE LOGIN and DEFAULT-IMAGE LOCK; never claim identical
results or physical adoption. These are shared login/lock assets, not native KDE
kscreenlockerrc Greeter/Wallpaper controls.

SDDM ThemeConfig reads at setThemePath/greeter construction. No watcher/reload
acknowledgement was found in inspected relevant source. Do not restart SDDM,
logout, launch preview/greeter, signal a live lock, or change selected theme.
Saved file verification is distinct from future/new-greeter/rendered behavior.

## Native authorization and failure hazards

Installed action `aeroshell.kcmaeroshellsddm.write`: allow_active=auth_admin,
allow_inactive=no. System bus service `aeroshell.kcmaeroshellsddm`, root,
Exec `/usr/lib/kf6/kauth/libexec/kcmaeroshellsddmhelper`. Bus policy permits root
ownership and default callers to send. KAuth is the authorization boundary;
permissive bus routing is not authorization.

The initial sandbox metadata snapshot reports nobody:nogroup (helper0755,
config/policy0644, dir0755). A later paired 13-path check outside the sandbox
shows root:root with the same modes: the sandbox maps root IDs to 65534.
Native host discovery verifies the fixed assets as trusted. No chown or asset
repair is needed or performed. The old helper is still unsuitable because of
the concrete API and transaction hazards below; the new protected helper/policy
and approval must be installed separately.

Existing helper:
* accepts arbitrary `path` and URL->local path, no fixed target, type whitelist,
  image validation, parent ownership/symlink/race checks, or revision CAS;
* removes old background before copying replacement; copy failure loses it;
* RDP removal fails if already absent (not idempotent);
* unrequested bool-only Apply can copy displayed default into absent background;
* writes four booleans separately after image operation; no lock, rollback,
  durable journal or collective transaction; sync failure can leave partial state;
* changes file permissions, no greeter acknowledgement/readback;
* no root-asset mutation is authorized by passive page construction.

## Concrete proposed provider/model contract (not yet implementation)

All four Bool controls plus explicit BackgroundIntent:
Keep / ReplaceImage / RemoveOverride (RDP) / RestoreApprovedImage. Keep source
image bytes/hash pinned independently of view lifetime; selecting a new image
must not be overwritten by an asynchronous refresh. Replacement is a local
bounded validated PNG/JPEG, never arbitrary remote URL or helper-selected path.
RDP=true removal and RDP=false choosing/restoring image are atomic draft
operations; untouched image must stay untouched on bool-only changes. Report
conflicting legacy `rdpBackground`/asset presence honestly. Removal success is
absence, idempotent. Restore image copies pinned `default-background`; it is
not equivalent to removing override for both consumers.

Snapshot/review includes: fixed theme/metadata/SDDM selection; actual base/user
INI bytes, inode/mode/owner/revisions and per-key raw/effective/presence/default
provenance; original/background/default file presence and byte digest; source
image digest/size/type/dimensions; approved Aero shell/AuthUI and reader bytes.
Transport the selected user-readable image as bounded pinned BYTES, not a caller
path opened with root privileges (which could disclose a root-only image).
Track source validity independently from permission to mutate system files.

Use a NEW root-owned fixed-path helper/polkit policy and packaging, analogous to
TimeSyncHelper: GNUInstallDirs compiled helper path, pkexec exact exec-path
annotation, org.linuxcontrol.<specific>.configure, active auth_admin and other
callers no, fixed minimal environment, root check, bounded structured input,
per-operation lock under /run/lock, exact owned key/intent whitelist, no shell,
no caller-selected destination or other theme. Recheck source identity/defaults/
policy/file revision AFTER authorization before mutation. Preserve other keys,
other asset files, default-background, selected theme, ownership/modes.

TrustedPath precedent src/time/TimeSyncConfig.cpp checks every ancestor with
lstat: root owner, no group/world writable components, no symlinks; missing known
leaf only. The host assets meet this ownership gate. The missing new helper,
policy and approval still require a controlled system installation; the frontend
must not accept a user-writable install prefix as trusted.

Stage new config/image without removing originals. Commit each file atomically
with safe descriptor-relative paths/no-follow identity checks; fsync file and
parent, CAS before replacement, durable journal and rollback/recovery for the
multi-file operation. Reader processes independently open two files: no true
cross-file atomic visibility is possible without consumer changes. Describe
this as guarded, recoverable per-file atomic commit, NOT all-files atomically
observed. Partial failure/unknown timeout stays uncertain and retains draft;
recovery must not overwrite foreign edits. Never blindly retry timed-out helper.
No SDDM restart/reload/greeter preview is needed or authorized for saved changes.

Recommended implementation sequence:
1. Agree typed model/intents, both consumer projections, truthful preview labels,
   provenance and defaults; injected controller tests first.
2. Build fixed-path transactional filesystem core with temporary roots, failure
   injection per stage, CAS/alias/symlink/nonregular/permissions/limits tests,
   lost-ack and crash-journal recovery tests; no native processes.
3. Add new root-owned helper/polkit packaging and verify installation capability;
   injected auth runner tests cover cancel, wrong helper, timeouts, duplicate/late
   results, source deletion/replacement, and retained drafts.
4. Whole custom form in Aero style: all4 bools, pinned target, source chooser,
   Remove/RDP and approved-default review, explicit shared-impact disclosure,
   staged differences and Apply/Revert/refresh/close guard; no theme switch.
5. Isolated fake end-to-end validation/read-only source evidence; live admin
   deployment, real preference write, login/lock reload/render/auth acceptance
   remain separate unverified obligations until explicitly performed.
